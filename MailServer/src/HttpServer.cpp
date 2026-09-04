// 用于实现 HTTP 服务器（Web 后端）的代码部分
// 负责：解析浏览器发来的 HTTP 请求 → 按 /api/xxx 路由 → 内部调 SMTP/POP3 客户端

#include "HttpServer.h"
#include "SmtpClient.h"
#include "Pop3Client.h"
#include "MailCrypto.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>

namespace {
const char* kMailServerIp = "127.0.0.1";
const int   kSmtpPort     = 2525;    // 本机 MailServer 的 SMTP 端口
const int   kPop3Port     = 1110;    // 本机 MailServer 的 POP3 端口
const int   kRecvTimeoutSec = 10;    // HTTP 请求读取超时（秒）

// ===================== 加密相关配置（预留接口） =====================
// 密钥：收发双方必须一致。以后做密钥管理时，可以改成从配置文件读。
const std::string kCryptoKey = "MailForge-Course-Key-2026";
// 发信时用哪种算法（encrypt=1 时生效）。
// 目前 XOR 已实现可跑；AES/RC4 见 MailCrypto.h 里的【TODO】
const MailCrypto::CryptoAlgo kCryptoAlgo = MailCrypto::ALGO_XOR;

// 一封邮件解码后的结果（无论明文还是密文，统一成可展示的形态）
struct DecodedMail {
    bool encrypted = false;   // 正文是否加密存储
    std::string from;         // 发件人
    std::string subject;      // 主题（加密邮件 = 解密后的主题）
    std::string display;      // 展示用完整文本（加密邮件已还原成明文）
};

// 把一封 POP3 拉回来的原始 .eml 解码：
//   明文邮件 → 原样展示；
//   加密邮件 → 正文带 MailForge::ENC::XOR:: 签名头 → 用密钥解密"正文载荷"，
//             载荷里还原出主题与正文，重新拼成可读邮件。
// 【TODO】换 AES/RC4 后只需在 MailCrypto::decryptPayload 里加分支，这里不动。
DecodedMail decodeMail(const std::string& raw) {
    DecodedMail dm;
    dm.from = HttpServer::parseHeader(raw, "From");

    // 1) 按空行把原始 .eml 切成 头部区 + 正文区
    const std::string sepCRLF = "\r\n\r\n";
    const std::string sepLF   = "\n\n";
    std::string headerPart = raw;
    std::string bodyPart;
    size_t sep = raw.find(sepCRLF);
    if (sep != std::string::npos) {
        headerPart = raw.substr(0, sep);
        bodyPart   = raw.substr(sep + sepCRLF.size());
    } else {
        sep = raw.find(sepLF);
        if (sep != std::string::npos) {
            headerPart = raw.substr(0, sep);
            bodyPart   = raw.substr(sep + sepLF.size());
        }
    }

    // 2) 判断正文是不是加密的（带签名头）
    const std::string magic(MailCrypto::kEncMagicXor);
    bool isCipher = bodyPart.compare(0, magic.size(), magic) == 0;

    if (!isCipher) {
        // ---- 明文邮件：原样展示 ----
        dm.display  = raw;
        dm.subject  = HttpServer::parseHeader(raw, "Subject");
        if (dm.subject.empty()) dm.subject = "(无主题)";
        return dm;
    }

    // ---- 加密邮件：解密正文载荷，还原出 Subject 与正文 ----
    dm.encrypted = true;
    std::string plain = MailCrypto::decryptPayload(bodyPart, kCryptoKey);

    // 载荷格式是我们发送时约定的："Subject: xxx\r\n\r\n正文..."
    std::string loadSubject = HttpServer::parseHeader(plain, "Subject");
    std::string loadBody    = plain;
    size_t ls = plain.find(sepCRLF);
    if (ls != std::string::npos) loadBody = plain.substr(ls + sepCRLF.size());
    else {
        ls = plain.find(sepLF);
        if (ls != std::string::npos) loadBody = plain.substr(ls + sepLF.size());
    }

    dm.subject = loadSubject.empty() ? "(加密邮件)" : loadSubject;

    // ---- 重建展示文本：保留原头部区的 Date/To 等所有字段 ----
    // 之前这里只拼了 From + Subject，导致加密邮件阅读时看不到 Date/To；
    // 现在改为：原样保留头部每一行，仅把占位的 Subject 换成解密后的真实主题。
    auto isSubjectHeader = [](const std::string& line) {
        const std::string key = "Subject";
        if (line.size() <= key.size() || line[key.size()] != ':') return false;
        for (size_t i = 0; i < key.size(); ++i) {
            if (tolower((unsigned char)line[i]) != tolower((unsigned char)key[i])) return false;
        }
        return true;
    };

    std::string newHeader;
    {
        std::istringstream iss(headerPart);
        std::string line;
        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            if (isSubjectHeader(line)) continue;   // 去掉占位的 Subject: [加密邮件]
            newHeader += line + "\r\n";
        }
    }

    dm.display = newHeader
               + "Subject: " + dm.subject + "\r\n"    // 换成解密后的真实主题
               + "\r\n" + loadBody;
    return dm;
}
} // namespace

// 构造函数：端口交给 Server 基类，确保 web 目录存在
HttpServer::HttpServer(int port) : Server(port) {
    mkdir("./web", 0755);   // 静态页面目录（web/index.html）
}

// ==================== HTTP 底层工具 ====================

// 读一行（遇到 \n 结束），去掉行尾 \r\n；失败返回 false
bool HttpServer::readLine(int fd, std::string& line) {
    line.clear();
    char c;
    while (true) {
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) return false;          // 连接关闭或出错/超时
        if (c == '\n') break;
        if (line.size() < 8192) line += c;
    }
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return true;
}

// 解析整个 HTTP 请求：请求行 + 头部 + （POST）body
bool HttpServer::readRequest(int fd, HttpRequest& req) {
    // 1) 请求行：如 "POST /api/login HTTP/1.1"，按空格拆成三份
    std::string requestLine;
    if (!readLine(fd, requestLine)) return false;
    std::istringstream lineStream(requestLine);
    std::string target;
    lineStream >> req.method >> target >> req.version;
    if (target.empty()) return false;

    // 2) 目标可能是 /path?query=1&x=2，拆出路径和查询参数
    size_t q = target.find('?');
    if (q != std::string::npos) {
        req.path = target.substr(0, q);
        parseKeyValues(target.substr(q + 1), req.query);
    } else {
        req.path = target;
    }

    // 3) 头部若干行，直到空行；顺便找 Content-Length
    int contentLength = 0;
    while (true) {
        std::string header;
        if (!readLine(fd, header)) return false;
        if (header.empty()) break;                       // 空行 = 头部结束
        size_t colon = header.find(':');
        if (colon == std::string::npos) continue;
        std::string name = header.substr(0, colon);
        std::string value = header.substr(colon + 1);
        while (!value.empty() && value.front() == ' ') value.erase(0, 1);   // 去前导空格
        if (name.size() == 14) {
            // Content-Length 大小写不敏感比较
            std::string lower;
            for (char c : name) lower += (char)tolower((unsigned char)c);
            if (lower == "content-length") contentLength = atoi(value.c_str());
        }
    }

    // 4) 读取 body（POST 表单）。Content-Length 为 0 时没有 body
    std::string body;
    while (contentLength > 0) {
        char buf[4096];
        int want = (contentLength < (int)sizeof(buf)) ? contentLength : (int)sizeof(buf);
        ssize_t n = recv(fd, buf, want, 0);
        if (n <= 0) return false;
        body.append(buf, n);
        contentLength -= (int)n;
    }

    // 5) POST body 是 a=b&c=d 表单，解析进 form
    if (req.method == "POST" && !body.empty()) {
        parseKeyValues(body, req.form);
    }
    return true;
}
// 解析 "a=b&c=d&e=f" 这种键值串（做 URL 解码）
void HttpServer::parseKeyValues(const std::string& raw,
                                std::map<std::string, std::string>& out) {
    size_t pos = 0;
    while (pos <= raw.size()) {
        size_t amp = raw.find('&', pos);
        std::string pair = (amp == std::string::npos) ? raw.substr(pos)
                                                      : raw.substr(pos, amp - pos);
        if (!pair.empty()) {
            size_t eq = pair.find('=');
            if (eq == std::string::npos) {
                out[urlDecode(pair)] = "";          // 无值的参数
            } else {
                out[urlDecode(pair.substr(0, eq))] = urlDecode(pair.substr(eq + 1));
            }
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
}

// URL 解码：+ → 空格，%XX → 对应字符
std::string HttpServer::urlDecode(const std::string& s) {
    std::string out;
    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        c = (char)tolower((unsigned char)c);
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return 0;
    };
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') {
            out += ' ';
        } else if (s[i] == '%' && i + 2 < s.size()) {
            out += (char)((hexVal(s[i + 1]) << 4) | hexVal(s[i + 2]));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

// 把响应（状态行+头+body）发回给浏览器
void HttpServer::sendHttp(int fd, const HttpResponse& resp) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << resp.status << " " << resp.statusText << "\r\n"
        << "Content-Type: " << resp.contentType << "\r\n"
        << "Content-Length: " << resp.body.size() << "\r\n"
        << "Connection: close\r\n"
        << "\r\n"
        << resp.body;

    std::string msg = oss.str();
    size_t sent = 0;
    while (sent < msg.size()) {          // 循环 send 保证发完
        ssize_t n = send(fd, msg.data() + sent, msg.size() - sent, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("[HTTP] send 失败");
            return;
        }
        sent += (size_t)n;
    }
}

// ==================== JSON 小工具 ====================

// JSON 字符串转义：把 " \ 换行等特殊字符转义，防止生成的 JSON 坏掉
std::string HttpServer::jsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                // 控制字符转成 \u00XX（一般不会出现，防御一下）
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// 生成 {"ok":... , "msg":...}
std::string HttpServer::jsonResult(bool ok, const std::string& msg) {
    return std::string("{\"ok\":") + (ok ? "true" : "false")
         + ",\"msg\":\"" + jsonEscape(msg) + "\"}";
}

// ==================== 会话管理 ====================

// 生成一个随机会话 token（时间戳 + 随机数 + 进程号，够演示用）
std::string HttpServer::randomToken() const {
    std::ostringstream oss;
    oss << std::hex << (unsigned)time(nullptr) << "-" << (unsigned)rand()
        << "-" << (unsigned)getpid();
    return oss.str();
}

// 创建会话：token 存进 sessions_
void HttpServer::makeSession(const std::string& user, const std::string& pass,
                             std::string& tokenOut) {
    Session s;
    s.user = user;
    s.pass = pass;
    tokenOut = randomToken();
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    sessions_[tokenOut] = s;
}

// 由 token 找回会话
bool HttpServer::loginAndGetSession(const std::string& token, Session& out) {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    auto it = sessions_.find(token);
    if (it == sessions_.end()) return false;
    out = it->second;
    return true;
}

// ==================== 主流程：一个浏览器连接 ====================

void HttpServer::handleClient(int client_fd) {
    // 设置读取超时：浏览器半天不发请求也不能让线程无限占着
    struct timeval tv;
    tv.tv_sec = kRecvTimeoutSec;
    tv.tv_usec = 0;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    HttpRequest req;
    HttpResponse resp;

    if (!readRequest(client_fd, req)) {
        resp.status = 400;
        resp.statusText = "Bad Request";
        resp.contentType = "text/plain; charset=utf-8";
        resp.body = "bad request";
    } else {
        route(req, resp);
    }

    sendHttp(client_fd, resp);
    close(client_fd);   // 每个请求一条连接，处理完就断开（Connection: close）
}

// ==================== 路由 ====================

void HttpServer::route(const HttpRequest& req, HttpResponse& resp) {
    // /api/ 开头的走 REST 接口，其它路径当静态文件（网页）
    if (req.path.compare(0, 5, "/api/") == 0) {
        handleApi(req, resp);
    } else {
        handleStatic(req, resp);
    }
}

// ==================== 静态文件（网页） ====================

void HttpServer::handleStatic(const HttpRequest& req, HttpResponse& resp) {
    std::string path = req.path;
    if (path == "/") path = "/index.html";          // 首页

    // 简单防路径穿越：不允许 ".."
    if (path.find("..") != std::string::npos) {
        resp.status = 403;
        resp.statusText = "Forbidden";
        resp.contentType = "text/plain";
        resp.body = "forbidden";
        return;
    }

    // 按扩展名决定 Content-Type
    std::string ext;
    size_t dot = path.find_last_of('.');
    if (dot != std::string::npos) ext = path.substr(dot);
    for (char& c : ext) c = (char)tolower((unsigned char)c);
    if (ext == ".html" || ext == ".htm")      resp.contentType = "text/html; charset=utf-8";
    else if (ext == ".css")                   resp.contentType = "text/css; charset=utf-8";
    else if (ext == ".js")                    resp.contentType = "application/javascript; charset=utf-8";
    else if (ext == ".png")                   resp.contentType = "image/png";
    else if (ext == ".jpg" || ext == ".jpeg") resp.contentType = "image/jpeg";
    else                                      resp.contentType = "text/plain";

    // 在 web/ 目录下找文件
    std::ifstream file("./web" + path, std::ios::in | std::ios::binary);
    if (!file.is_open()) {
        resp.status = 404;
        resp.statusText = "Not Found";
        resp.contentType = "text/plain; charset=utf-8";
        resp.body = "404 Not Found";
        return;
    }
    resp.body.assign((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());
    resp.status = 200;
    resp.statusText = "OK";
}

// ==================== 从邮件原文里取头部字段 ====================

// 在头部区（第一个空行之前）找 "name:" 开头的行，返回它的值
std::string HttpServer::parseHeader(const std::string& rawMail, const std::string& name) {
    std::istringstream iss(rawMail);
    std::string line;
    std::string key = name + ":";
    std::string value;
    bool found = false;

    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;   // 空行 = 头部区结束，后面的都是正文

        // 续行：已找到该头后，下一行以空格/Tab 开头则属于它的值（RFC 5322 折叠头）
        if (found && (line[0] == ' ' || line[0] == '\t')) {
            value += " ";
            size_t b = line.find_first_not_of(" \t");
            if (b != std::string::npos) value += line.substr(b);
            continue;
        }
        // 大小写不敏感比较前几个字符是不是 "name:"
        if (line.size() > key.size()) {
            bool match = true;
            for (size_t i = 0; i < key.size(); ++i) {
                if (tolower((unsigned char)line[i]) != tolower((unsigned char)key[i])) {
                    match = false;
                    break;
                }
            }
            if (match) {
                value = line.substr(key.size());
                while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                    value.erase(0, 1);   // 去掉值前面的空白
                }
                found = true;
            }
        }
    }
    return value;
}

// ==================== 注册辅助（文件内匿名命名空间） ====================
namespace {
std::mutex gRegisterMutex;   // 注册接口并发保护：检查重名 + 写文件要整体原子进行

// 去首尾空白
std::string trimWs(const std::string& s) {
    size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t");
    return s.substr(b, e - b + 1);
}

// 规范化注册用户名：去空白 → 去 @ 域名 → 转小写，且只能由 字母/数字/_- . 组成
std::string normalizeRegUser(const std::string& input) {
    std::string u = trimWs(input);
    size_t at = u.find('@');
    if (at != std::string::npos) u = u.substr(0, at);
    for (char& c : u) c = (char)tolower((unsigned char)c);
    if (u.empty()) return "";
    for (char c : u) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
               || c == '-' || c == '_' || c == '.';
        if (!ok) return "";
    }
    if (u[0] == '.' || u.find("..") != std::string::npos) return "";
    return u;
}

// users.txt 里是否已存在该用户名（逐行比较规范化后的名字）
bool userExistsInFile(const std::string& user) {
    std::ifstream f("./users.txt");
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string t = trimWs(line);
        if (t.empty() || t[0] == '#') continue;
        size_t c = t.find(':');
        if (c == std::string::npos) continue;
        if (normalizeRegUser(t.substr(0, c)) == user) return true;
    }
    return false;
}

// 把 用户名:密码 追加写进 users.txt
bool appendUserToFile(const std::string& user, const std::string& pass) {
    std::ofstream f("./users.txt", std::ios::app);
    if (!f) return false;
    f << user << ":" << pass << "\n";
    return f.good();
}
} // namespace

// ==================== REST API 入口 ====================

// 根据 method + path 把请求分发给对应 handler
void HttpServer::handleApi(const HttpRequest& req, HttpResponse& resp) {
    resp.contentType = "application/json; charset=utf-8";

    if (req.method == "POST" && req.path == "/api/register") {
        handleRegister(req, resp);
    } else if (req.method == "POST" && req.path == "/api/login") {
        handleLogin(req, resp);
    } else if (req.method == "POST" && req.path == "/api/logout") {
        handleLogout(req, resp);
    } else if (req.method == "POST" && req.path == "/api/send") {
        handleSend(req, resp);
    } else if (req.method == "POST" && req.path == "/api/delete") {
        handleDelete(req, resp);
    } else if (req.method == "GET" && req.path == "/api/inbox") {
        handleInbox(req, resp);
    } else if (req.method == "GET" && req.path == "/api/mail") {
        handleMail(req, resp);
    } else {
        resp.body = jsonResult(false, "未知接口: " + req.method + " " + req.path);
    }
}

// 参数取值工具：先从 form（POST）取，取不到再从 query（GET）取
static std::string getParam(const HttpRequest& req, const std::string& key) {
    auto it = req.form.find(key);
    if (it != req.form.end()) return it->second;
    auto q = req.query.find(key);
    if (q != req.query.end()) return q->second;
    return "";
}

// ==================== POST /api/register ====================
// 参数：user, pass
// 流程：校验 → 查重 → 追加写 users.txt → 建收件目录 → 自动登录返回 token
// （POP3 登录时会自动重读 users.txt，所以新账号立即生效，无需重启服务器）
void HttpServer::handleRegister(const HttpRequest& req, HttpResponse& resp) {
    std::string user = getParam(req, "user");
    std::string pass = getParam(req, "pass");
    if (user.empty() || pass.empty()) {
        resp.body = jsonResult(false, "缺少 user 或 pass 参数");
        return;
    }
    if (pass.size() < 4) {
        resp.body = jsonResult(false, "密码太短，至少 4 位");
        return;
    }

    std::string regUser;
    {
        std::lock_guard<std::mutex> lock(gRegisterMutex);   // 查重 + 写文件要连续
        regUser = normalizeRegUser(user);
        if (regUser.empty()) {
            resp.body = jsonResult(false,
                "用户名只能由字母/数字/_-.组成，且不能以点开头");
            return;
        }
        if (userExistsInFile(regUser)) {
            resp.body = jsonResult(false, "该用户名已被注册");
            return;
        }
        if (!appendUserToFile(regUser, pass)) {
            resp.body = jsonResult(false,
                "写入 users.txt 失败：请确认在 MailServer 目录下运行");
            return;
        }
    }

    // 创建该用户的收件目录（如 ./mailbox/carol/）
    mkdir(("./mailbox/" + regUser).c_str(), 0755);

    // 自动登录：直接发 token（POP3 登录会自动重读 users.txt）
    std::string token;
    makeSession(regUser, pass, token);
    std::cout << "[HTTP] 新用户注册成功: " << regUser << std::endl;

    resp.body = std::string("{\"ok\":true,\"token\":\"") + token
              + "\",\"msg\":\"注册成功，已自动登录\"}";
}

// ==================== POST /api/login ====================
// 用 POP3 客户端真的去连 1110 试登录：能过 = 账号有效
void HttpServer::handleLogin(const HttpRequest& req, HttpResponse& resp) {
    std::string user = getParam(req, "user");
    std::string pass = getParam(req, "pass");
    if (user.empty() || pass.empty()) {
        resp.body = jsonResult(false, "缺少 user 或 pass 参数");
        return;
    }

    Pop3Client checker(kMailServerIp, kPop3Port);
    if (!checker.login(user, pass)) {
        resp.body = jsonResult(false, "登录失败: " + checker.getLastError());
        return;
    }
    checker.quit();

    // 登录成功：建会话，返回 token（后续收信/删信用它）
    std::string token;
    makeSession(user, pass, token);
    std::cout << "[HTTP] 用户 " << user << " 通过 Web 登录" << std::endl;

    resp.body = std::string("{\"ok\":true,\"token\":\"") + token + "\"}";
}

// ==================== POST /api/logout ====================
void HttpServer::handleLogout(const HttpRequest& req, HttpResponse& resp) {
    std::string token = getParam(req, "token");
    if (token.empty()) {
        resp.body = jsonResult(false, "缺少 token 参数");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        sessions_.erase(token);
    }
    resp.body = jsonResult(true, "已退出登录");
}

// ==================== POST /api/send ====================
// 参数：token, to, subject, body, from(可选), encrypt(可选，1=加密通道)
void HttpServer::handleSend(const HttpRequest& req, HttpResponse& resp) {
    Session session;
    if (!loginAndGetSession(getParam(req, "token"), session)) {
        resp.body = jsonResult(false, "token 无效或已过期，请先登录");
        return;
    }

    std::string to      = getParam(req, "to");
    std::string subject = getParam(req, "subject");
    std::string body    = getParam(req, "body");
    if (to.empty() || subject.empty()) {
        resp.body = jsonResult(false, "缺少 to / subject 参数");
        return;
    }
    // 发件人默认 <登录名>@example.com，也可以显式传 from
    std::string from = getParam(req, "from");
    if (from.empty()) from = session.user + "@example.com";

    bool wantEncrypt = (getParam(req, "encrypt") == "1");

    // ---- 拼一封标准邮件原文（头部区 + 空行 + 正文）----
    std::string mailSubject = subject;
    std::string mailBody    = body + "\r\n";
    if (wantEncrypt) {
        // ===================== 加密挂钩点①（发送前） =====================
        // 加密时把"主题 + 正文"整体打包成一个载荷并加密，放进邮件正文区；
        // 头部只留 "Subject: [加密邮件]" 占位（信封地址 From/To 必须明文才能投递）。
        // 收件方读取时 decodeMail() 会用同一把密钥自动解密还原主题与正文。
        // 【TODO】换 AES/RC4：改 MailCrypto::encryptPayload 内的算法分支即可，这里不动。
        mailSubject = "[加密邮件]";
        std::string envelope = "Subject: " + subject + "\r\n\r\n" + body;
        mailBody = MailCrypto::encryptPayload(envelope, kCryptoKey, kCryptoAlgo) + "\r\n";
    }

    std::string plain =
        "From: " + from + "\r\n"
        "To: " + to + "\r\n"
        "Subject: " + mailSubject + "\r\n"
        "\r\n" + mailBody;

    SmtpClient smtp(kMailServerIp, kSmtpPort);
    if (!smtp.sendRawMail(from, to, plain)) {
        resp.body = jsonResult(false, "SMTP 发送失败: " + smtp.getLastError());
        return;
    }

    std::cout << "[HTTP] " << session.user << " 发送邮件给 " << to
              << (wantEncrypt ? "（已加密）" : "") << std::endl;
    resp.body = jsonResult(true, wantEncrypt ? "发送成功（已加密存储）"
                                             : "发送成功");
}

// ==================== GET /api/inbox ====================
// 参数：token
// 返回：{"ok":true,"mails":[{number,size,subject,from,encrypted},...]}
void HttpServer::handleInbox(const HttpRequest& req, HttpResponse& resp) {
    Session session;
    if (!loginAndGetSession(getParam(req, "token"), session)) {
        resp.body = jsonResult(false, "token 无效或已过期，请先登录");
        return;
    }

    // 用该用户的账号开一条 POP3 会话
    Pop3Client pop3(kMailServerIp, kPop3Port);
    if (!pop3.login(session.user, session.pass)) {
        resp.body = jsonResult(false, "POP3 登录失败: " + pop3.getLastError());
        return;
    }

    std::vector<Pop3MailInfo> mails;
    if (!pop3.list(mails)) {
        resp.body = jsonResult(false, "POP3 LIST 失败: " + pop3.getLastError());
        return;
    }

    // 组装 JSON：为了在收件箱直接显示主题/发件人，逐封 RETR 并解析头部
    std::string json = "{\"ok\":true,\"mails\":[";
    for (size_t i = 0; i < mails.size(); ++i) {
        std::string raw;
        if (!pop3.retr(mails[i].number, raw)) continue;

        // ===================== 加密挂钩点②（收取后解密） =====================
        // decodeMail 会自动识别"正文加密"的邮件并还原主题/正文（见匿名命名空间实现）
        DecodedMail decoded = decodeMail(raw);
        std::string subject = decoded.subject;
        std::string from    = decoded.from;
        bool encrypted      = decoded.encrypted;

        if (i > 0) json += ",";
        json += "{\"number\":" + std::to_string(mails[i].number)
              + ",\"size\":" + std::to_string(mails[i].size)
              + ",\"subject\":\"" + jsonEscape(subject)
              + "\",\"from\":\"" + jsonEscape(from)
              + "\",\"encrypted\":" + (encrypted ? "true" : "false") + "}";
    }
    json += "]}";

    pop3.quit();   // 注意：没 DELE 任何邮件，服务器上的信不会丢（网页收信≠删信）
    resp.body = json;
}

// ==================== GET /api/mail ====================
// 参数：token, n（邮件编号）
// 返回：{"ok":true, "number":n, "encrypted":bool, "raw":"完整邮件原文"}
void HttpServer::handleMail(const HttpRequest& req, HttpResponse& resp) {
    Session session;
    if (!loginAndGetSession(getParam(req, "token"), session)) {
        resp.body = jsonResult(false, "token 无效或已过期，请先登录");
        return;
    }
    int number = atoi(getParam(req, "n").c_str());
    if (number <= 0) {
        resp.body = jsonResult(false, "缺少合法的 n（邮件编号）参数");
        return;
    }

    Pop3Client pop3(kMailServerIp, kPop3Port);
    if (!pop3.login(session.user, session.pass)) {
        resp.body = jsonResult(false, "POP3 登录失败: " + pop3.getLastError());
        return;
    }

    std::string raw;
    if (!pop3.retr(number, raw)) {
        pop3.quit();
        resp.body = jsonResult(false, "读取第 " + std::to_string(number)
                                    + " 封失败: " + pop3.getLastError());
        return;
    }
    pop3.quit();

    // ===================== 加密挂钩点③（读单封时解密） =====================
    DecodedMail decoded = decodeMail(raw);

    resp.body = "{\"ok\":true,\"number\":" + std::to_string(number)
              + ",\"encrypted\":" + (decoded.encrypted ? "true" : "false")
              + ",\"raw\":\"" + jsonEscape(decoded.display) + "\"}";
}

// ==================== POST /api/delete ====================
// 参数：token, n（邮件编号）
void HttpServer::handleDelete(const HttpRequest& req, HttpResponse& resp) {
    Session session;
    if (!loginAndGetSession(getParam(req, "token"), session)) {
        resp.body = jsonResult(false, "token 无效或已过期，请先登录");
        return;
    }
    int number = atoi(getParam(req, "n").c_str());
    if (number <= 0) {
        resp.body = jsonResult(false, "缺少合法的 n（邮件编号）参数");
        return;
    }

    Pop3Client pop3(kMailServerIp, kPop3Port);
    if (!pop3.login(session.user, session.pass)) {
        resp.body = jsonResult(false, "POP3 登录失败: " + pop3.getLastError());
        return;
    }
    bool ok = pop3.dele(number);
    // QUIT 才真正删除
    pop3.quit();
    if (!ok) {
        resp.body = jsonResult(false, "删除第 " + std::to_string(number)
                                    + " 封失败: " + pop3.getLastError());
        return;
    }
    std::cout << "[HTTP] " << session.user << " 删除了第 "
              << number << " 封邮件" << std::endl;
    resp.body = jsonResult(true, "已删除第 " + std::to_string(number) + " 封");
}

