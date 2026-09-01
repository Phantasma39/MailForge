// ============================================================================
//  SmtpClientWeb/bridge.cpp
//  浏览器 <-> SMTP 服务器 的桥接服务（C++ 手写 HTTP 服务器）
//
//  为什么需要它：浏览器里的 JS 无法直接建立 TCP 连接，
//  所以需要一个本地桥：浏览器(HTTP) ─► bridge(TCP) ─► SMTP 服务器(127.0.0.1:2525)
//
//  接口：
//    GET  /                     → 静态网页（www/ 目录）
//    POST /api/connect          → 建立到 SMTP 服务器的连接，返回 220 问候语
//    POST /api/command          → 发送一行 SMTP 命令（或 DATA 多行正文），返回服务器响应
//    POST /api/disconnect       → 发送 QUIT 并关闭会话
//
//  编译： g++ -std=c++17 -O2 -pthread -o bridge bridge.cpp
// ============================================================================
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// ---------------------- 可配置参数 ----------------------
static const int   HTTP_PORT = 8888;         // 网页 / 桥接端口
static const char* SMTP_HOST = "127.0.0.1";  // SMTP 服务器地址
static const int   SMTP_PORT = 2525;         // 与 MailServer/main.cpp 保持一致

// ---------------------- SMTP 会话 ----------------------
struct SmtpSession {
    int         sock     = -1;    // 到 SMTP 服务器的 socket
    std::string pending;          // 行缓冲（TCP 是流式协议，需要按 \n 攒行）
    bool        dataMode = false; // 是否处于 DATA 模式（服务器正在等待正文）
    bool        ended    = false; // 会话是否已结束（收到 221 / 连接被关闭）
};

static std::mutex                 g_mtx;
static std::map<int, SmtpSession> g_sessions;
static int                        g_next_id = 1;
static std::string                g_www_dir;   // 静态网页目录（main 里初始化）

// ---------------------- 基础工具 ----------------------

// 获取可执行文件所在目录（用于定位 www 静态目录，与运行目录无关）
static std::string exe_dir(const char* argv0) {
    std::string dir = (argv0 && argv0[0]) ? argv0 : "./bridge";
    if (dir[0] != '/') dir = "./" + dir;
    size_t pos = dir.find_last_of('/');
    if (pos != std::string::npos) dir = dir.substr(0, pos);
    return dir.empty() ? "." : dir;
}

static bool send_all(int fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = send(fd, data.data() + sent, data.size() - sent, 0);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

// 读取一行（带超时）。失败时：超时 → closed=false；连接断开/出错 → closed=true
static bool read_line(int fd, std::string& pending, std::string& line,
                      int timeout_ms, bool* closed = nullptr) {
    if (closed) *closed = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (true) {
        size_t pos = pending.find('\n');
        if (pos != std::string::npos) {
            line = pending.substr(0, pos);
            pending.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return true;
        }
        int remain = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                         deadline - std::chrono::steady_clock::now()).count();
        if (remain <= 0) return false;               // 超时
        struct pollfd pfd = {fd, POLLIN, 0};
        int r = poll(&pfd, 1, remain);
        if (r < 0) { if (closed) *closed = true; return false; }
        if (r == 0) return false;                    // 超时
        char buf[4096];
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) { if (closed) *closed = true; return false; }
        pending.append(buf, (size_t)n);
    }
}

// 读取一条完整的 SMTP 响应（兼容多行响应："250-xxx" ... "250 xxx"）
static std::string read_full_response(int fd, std::string& pending) {
    std::string out;
    std::string line;
    while (true) {
        bool closed = false;
        if (!read_line(fd, pending, line, 3000, &closed)) {
            out += closed ? "（服务器已关闭连接）" : "ERROR: 等待服务器响应超时（3 秒）";
            break;
        }
        out += line;
        out += '\n';
        // 形如 "354 "、"250 "、"221 "（三码+空格）：是响应的最后一行
        if (line.size() >= 4 &&
            isdigit((unsigned char)line[0]) &&
            isdigit((unsigned char)line[1]) &&
            isdigit((unsigned char)line[2]) &&
            (line[3] == ' ' || line[3] == '\t')) {
            break;
        }
        // 形如 "250-"：多行响应，继续读下一行
    }
    return out;
}

static bool open_smtp_connection(SmtpSession& s) {
    s.sock = socket(AF_INET, SOCK_STREAM, 0);
    if (s.sock < 0) return false;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)SMTP_PORT);
    if (inet_pton(AF_INET, SMTP_HOST, &addr.sin_addr) != 1) {
        close(s.sock); s.sock = -1;
        return false;
    }
    if (connect(s.sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(s.sock); s.sock = -1;
        return false;
    }
    return true;
}

// ---------------------- HTTP 请求解析 ----------------------
struct HttpRequest {
    std::string method;
    std::string path;
    std::string query;
    std::string body;
};

static bool parse_request(int fd, HttpRequest& req) {
    std::string raw;
    while (raw.find("\r\n\r\n") == std::string::npos) {
        char buf[4096];
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        raw.append(buf, (size_t)n);
        if (raw.size() > 1024 * 1024) return false;
    }
    size_t head_end = raw.find("\r\n\r\n");
    std::string head = raw.substr(0, head_end);

    std::istringstream hs(head);
    std::string target, version;
    hs >> req.method >> target >> version;
    size_t q = target.find('?');
    if (q == std::string::npos) req.path = target;
    else { req.path = target.substr(0, q); req.query = target.substr(q + 1); }

    size_t cl = 0;
    size_t p = head.find("\r\n");
    std::istringstream h2(p == std::string::npos ? "" : head.substr(p));
    std::string hline;
    while (std::getline(h2, hline)) {
        if (!hline.empty() && hline.back() == '\r') hline.pop_back();
        size_t colon = hline.find(':');
        if (colon == std::string::npos) continue;
        std::string name = hline.substr(0, colon);
        std::string value = hline.substr(colon + 1);
        for (auto& c : name) c = (char)tolower((unsigned char)c);
        if (name == "content-length") cl = (size_t)strtoul(value.c_str(), nullptr, 10);
    }

    req.body = raw.substr(head_end + 4);
    while (req.body.size() < cl) {
        char buf[4096];
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        req.body.append(buf, (size_t)n);
    }
    return true;
}

static std::string content_type_for(const std::string& path) {
    if (path.size() > 5 && path.compare(path.size() - 5, 5, ".html") == 0) return "text/html; charset=utf-8";
    if (path.size() > 4 && path.compare(path.size() - 4, 4, ".css") == 0) return "text/css; charset=utf-8";
    if (path.size() > 3 && path.compare(path.size() - 3, 3, ".js") == 0) return "application/javascript; charset=utf-8";
    if (path.size() > 4 && path.compare(path.size() - 4, 4, ".svg") == 0) return "image/svg+xml";
    return "application/octet-stream";
}

static std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::ostringstream oss;
    oss << f.rdbuf();
    return oss.str();
}

static int session_id_from_query(const std::string& query) {
    size_t p = query.find("session=");
    if (p == std::string::npos) return -1;
    return atoi(query.c_str() + p + 8);
}

static void send_http_response(int fd, const std::string& status, const std::string& content_type,
                               const std::string& body, const std::string& extra_headers = "") {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << status << "\r\n";
    oss << "Content-Type: " << content_type << "\r\n";
    oss << "Content-Length: " << body.size() << "\r\n";
    if (!extra_headers.empty()) oss << extra_headers;   // 需以 \r\n 结尾，如 "X-Session: 3\r\n"
    oss << "Connection: close\r\n\r\n";
    oss << body;
    send_all(fd, oss.str());
}

// 处理一个 HTTP 连接（每连接一个线程）
static void handle_connection(int client_fd) {
    HttpRequest req;
    if (!parse_request(client_fd, req)) {
        close(client_fd);
        return;
    }

    std::string status = "200 OK";
    std::string content_type = "text/plain; charset=utf-8";
    std::string body;
    std::string extra;   // 附加响应头，例如 "X-Session: 3\r\n"

    if (req.method == "GET") {
        // ---------------- 静态文件 ----------------
        std::string path = req.path.empty() ? "/index.html" : req.path;
        if (path == "/") path = "/index.html";
        if (path.find("..") != std::string::npos) {
            status = "400 Bad Request";
            body = "非法路径";
        } else {
            std::string full = g_www_dir + path;
            body = read_file(full);
            if (body.empty()) {
                status = "404 Not Found";
                body = "404 Not Found: " + path;
            } else {
                content_type = content_type_for(path);
            }
        }
    } else if (req.method == "POST") {
        // ---------------- /api/connect ----------------
        if (req.path == "/api/connect") {
            std::lock_guard<std::mutex> lk(g_mtx);
            int id = g_next_id++;
            g_sessions[id] = SmtpSession();
            SmtpSession& sess = g_sessions[id];
            if (!open_smtp_connection(sess)) {
                g_sessions.erase(id);
                status = "502 Bad Gateway";
                body = "无法连接 SMTP 服务器 " + std::string(SMTP_HOST) + ":" +
                       std::to_string(SMTP_PORT) + "\n请先启动 MailServer 的 smtp_server";
            } else {
                body = read_full_response(sess.sock, sess.pending);
                extra = "X-Session: " + std::to_string(id) + "\r\n";
            }
        }
        // ---------------- /api/command ----------------
        else if (req.path == "/api/command") {
            int id = session_id_from_query(req.query);
            std::lock_guard<std::mutex> lk(g_mtx);
            auto it = g_sessions.find(id);
            if (it == g_sessions.end() || it->second.sock < 0) {
                status = "400 Bad Request";
                body = "会话不存在或已断开，请先调用 /api/connect";
            } else if (it->second.ended) {
                status = "421 Session ended";
                body = "421 会话已结束（QUIT 后连接已关闭），请重新连接";
            } else {
                SmtpSession& sess = it->second;
                // 去掉 body 末尾的换行
                std::string payload = req.body;
                while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
                    payload.pop_back();

                if (sess.dataMode) {
                    // ---- DATA 模式：把正文按行发送，最后自动补一行 "." 结束 ----
                    std::vector<std::string> lines;
                    std::istringstream ls(payload);
                    std::string l;
                    while (std::getline(ls, l)) {
                        while (!l.empty() && l.back() == '\r') l.pop_back();
                        lines.push_back(l);
                    }
                    bool ok = true;
                    for (const std::string& ln : lines) {
                        if (!send_all(sess.sock, ln + "\r\n")) { ok = false; break; }
                    }
                    if (ok && (lines.empty() || lines.back() != "."))
                        ok = send_all(sess.sock, ".\r\n");
                    if (!ok) {
                        status = "502 Bad Gateway";
                        body = "发送失败（SMTP 连接可能已断开）";
                        sess.ended = true;
                        close(sess.sock); sess.sock = -1;
                    } else {
                        body = read_full_response(sess.sock, sess.pending);
                        sess.dataMode = false;
                        if (body.compare(0, 3, "221") == 0 ||
                            body.find("服务器已关闭连接") != std::string::npos)
                            sess.ended = true;
                    }
                } else {
                    // ---- 普通单行命令 ----
                    if (!send_all(sess.sock, payload + "\r\n")) {
                        status = "502 Bad Gateway";
                        body = "发送失败（SMTP 连接可能已断开）";
                        sess.ended = true;
                        close(sess.sock); sess.sock = -1;
                    } else {
                        body = read_full_response(sess.sock, sess.pending);
                        // 服务器要求进入 DATA 模式（354）时记录状态
                        if (body.compare(0, 3, "354") == 0) sess.dataMode = true;
                        if (body.compare(0, 3, "221") == 0 ||
                            body.find("服务器已关闭连接") != std::string::npos)
                            sess.ended = true;
                    }
                }
            }
        }

        // ---------------- /api/disconnect ----------------
        else if (req.path == "/api/disconnect") {
            int id = session_id_from_query(req.query);
            std::lock_guard<std::mutex> lk(g_mtx);
            auto it = g_sessions.find(id);
            if (it != g_sessions.end()) {
                SmtpSession& sess = it->second;
                if (sess.sock >= 0) {
                    if (!sess.ended) {
                        send_all(sess.sock, "QUIT\r\n");
                        body = read_full_response(sess.sock, sess.pending);
                    } else {
                        body = "221 Bye（会话已在 QUIT 时结束）";
                    }
                    close(sess.sock);
                    sess.sock = -1;
                }
                g_sessions.erase(it);
            }
            if (body.empty()) body = "会话不存在";
        }
        // ---------------- 未知接口 ----------------
        else {
            status = "404 Not Found";
            body = "未找到接口: " + req.path;
        }
    } else {
        status = "405 Method Not Allowed";
        body = "仅支持 GET / POST";
    }

    send_http_response(client_fd, status, content_type, body, extra);
    close(client_fd);
}

// ---------------------- 入口 ----------------------
int main(int argc, char** argv) {
    (void)argc;
    g_www_dir = exe_dir(argv[0]) + "/www";

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return 1; }
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;   // 监听所有网卡，方便局域网 / ZeroTier 访问
    addr.sin_port = htons((uint16_t)HTTP_PORT);
    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind 失败（8888 端口被占用？）");
        close(listen_fd);
        return 1;
    }
    if (listen(listen_fd, 32) < 0) { perror("listen"); close(listen_fd); return 1; }

    std::cout << "==================================================\n";
    std::cout << "  浏览器 <-> SMTP 桥接服务已启动\n";
    std::cout << "  打开网页:  http://localhost:" << HTTP_PORT << "\n";
    std::cout << "  目标 SMTP: " << SMTP_HOST << ":" << SMTP_PORT << "\n";
    std::cout << "  静态目录:  " << g_www_dir << "\n";
    std::cout << "==================================================\n";

    while (true) {
        int cfd = accept(listen_fd, nullptr, nullptr);
        if (cfd < 0) { perror("accept"); continue; }
        std::thread([](int fd) { handle_connection(fd); }, cfd).detach();
    }
}



