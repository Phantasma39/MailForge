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
#include <chrono>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cerrno>

namespace {
const char* kMailServerIp = "127.0.0.1";
const int   kSmtpPort     = 2525;    // 本机 MailServer 的 SMTP 端口
const int   kPop3Port     = 1110;    // 本机 MailServer 的 POP3 端口
const int   kRecvTimeoutSec = 10;    // HTTP 请求读取超时（秒）

// ===================== 加密相关配置 =====================
// ★ 当前加密通道：自研对称加密（AES-256-CBC / ChaCha20，RFC 8439），见
//   MailCrypto.h。每个账号一把 32 字节对称密钥文件 keys/<用户名>.key，
//   首次使用自动生成。发信用收件人密钥加密，收信用本人密钥解密。

// 一封邮件解码后的结果（无论明文还是密文，统一成可展示的形态）
struct DecodedMail {
    bool encrypted = false;   // 正文是否加密存储
    std::string from;         // 发件人
    std::string subject;      // 主题（加密邮件 = 解密后的主题）
    std::string display;      // 展示用完整文本（加密邮件已还原成明文）
};

// 把"解密后载荷"合并进展示结果。载荷格式（发送端约定）：
//   "Subject: 真实主题\r\n\r\n正文区..."
// 保留原 .eml 头部区的 Date/To/From 等字段，仅去掉占位 Subject 与加密标记行，
// 再把主题替换为解密出的真实主题、正文替换为解出的正文。
void restoreFromPayload(DecodedMail& dm, const std::string& plain,
                        const std::string& headerPart) {
    const std::string sepCRLF = "\r\n\r\n";
    const std::string sepLF   = "\n\n";

    std::string loadSubject = HttpServer::parseHeader(plain, "Subject");
    std::string loadBody    = plain;
    size_t ls = plain.find(sepCRLF);
    if (ls != std::string::npos) loadBody = plain.substr(ls + sepCRLF.size());
    else {
        ls = plain.find(sepLF);
        if (ls != std::string::npos) loadBody = plain.substr(ls + sepLF.size());
    }
    dm.subject = loadSubject.empty() ? "(加密邮件)" : loadSubject;

    // 字段名大小写不敏感比较（RFC 5322：字段名不区分大小写）
    auto fieldNameIs = [](const std::string& line, const std::string& name) {
        if (line.size() <= name.size() || line[name.size()] != ':') return false;
        for (size_t i = 0; i < name.size(); ++i) {
            if (tolower((unsigned char)line[i]) != tolower((unsigned char)name[i])) {
                return false;
            }
        }
        return true;
    };

    // 重建头部：保留原样，去掉占位 Subject 与本系统加密标记行
    std::string newHeader;
    {
        std::istringstream iss(headerPart);
        std::string line;
        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            if (fieldNameIs(line, "Subject")) continue;              // 去掉占位 Subject
            if (fieldNameIs(line, "X-MailForge-Crypto")) continue;   // 去掉加密标记
            newHeader += line + "\r\n";
        }
    }

    dm.display = newHeader
               + "Subject: " + dm.subject + "\r\n"    // 换成解密后的真实主题
               + "\r\n" + loadBody;
}

// 把一封 POP3 拉回来的原始 .eml 解码成可展示形态。
// viewerUser 是"正在查看邮件的收件人"（加密邮件用他自己账号的密钥解密）。
// 支持形态：
//   1) 明文邮件          → 原样展示；
//   2) 自研加密(AES/CHA) → 正文带 MailForge::ENC::AES/CHA 魔数头 → 自动解密。
DecodedMail decodeMail(const std::string& raw, const std::string& viewerUser) {
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

    // 2) ★ 自研加密邮件：正文带 AES/ChaCha20 魔数头，用收件人账号密钥还原
    if (MailCrypto::isEncryptedText(bodyPart)) {
        dm.encrypted = true;
        std::string key;
        std::string plain;
        if (MailCrypto::getUserKey(viewerUser, key) &&
            MailCrypto::decryptPayload(bodyPart, key, plain)) {
            restoreFromPayload(dm, plain, headerPart);
            return dm;
        }

        // 解密失败：密钥缺失/不匹配/密文被篡改等情况
        dm.subject = "(加密邮件，无法解密)";
        dm.display = headerPart + "\r\n\r\n" + bodyPart;   // 原文保留可导出
        return dm;
    }

    // 3) 明文邮件：原样展示
    dm.display = raw;
    dm.subject = HttpServer::parseHeader(raw, "Subject");
    if (dm.subject.empty()) dm.subject = "(无主题)";
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
        << resp.extraHeaders
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


// ==================== 附件 / MIME 辅助（文件内匿名命名空间） ====================
namespace {

// 把 CRLF 统一成 \n（解析 MIME 时更省事）
std::string toLfText(const std::string& t) {
    std::string out;
    out.reserve(t.size());
    for (char c : t) if (c != '\r') out += c;
    return out;
}

std::string trimWsText(const std::string& s) {
    size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t");
    return s.substr(b, e - b + 1);
}

// 在头部区文本里取某个字段的值（大小写不敏感，取到空行停）
std::string partHeaderValue(const std::string& block, const std::string& name) {
    std::istringstream iss(block);
    std::string line;
    std::string key = name + ":";
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        // 折叠续行：值可能跨多行，这里简单只取首行（附件头基本够用）
        if (line.size() > key.size()) {
            bool match = true;
            for (size_t i = 0; i < key.size(); ++i) {
                if (tolower((unsigned char)line[i]) != tolower((unsigned char)key[i])) { match = false; break; }
            }
            if (match) {
                std::string v = line.substr(key.size());
                size_t b = v.find_first_not_of(" \t");
                if (b != std::string::npos) v = v.substr(b);
                return v;
            }
        }
    }
    return "";
}

// 从 Content-Disposition 里取 filename="xxx" 或 filename=xxx
std::string parseFilename(const std::string& disposition) {
    size_t p = disposition.find("filename");
    if (p == std::string::npos) return "";
    std::string rest = disposition.substr(p + 8);
    size_t eq = rest.find('=');
    if (eq == std::string::npos) return "";
    rest = rest.substr(eq + 1);
    rest = trimWsText(rest);
    if (!rest.empty() && rest[0] == '"') {
        size_t q = rest.find('"', 1);
        return q == std::string::npos ? rest.substr(1) : rest.substr(1, q - 1);
    }
    size_t semi = rest.find(';');
    return trimWsText(semi == std::string::npos ? rest : rest.substr(0, semi));
}

// 从邮件正文里找第一个 boundary（在头部 Content-Type 里，或正文第一行 --xxx）
bool findBoundary(const std::string& headerText, const std::string& bodyText, std::string& boundary) {
    // 1) 先看头部 Content-Type: multipart/...; boundary="..."
    std::string ct = partHeaderValue(headerText, "Content-Type");
    size_t p = ct.find("boundary");
    if (p != std::string::npos) {
        std::string rest = ct.substr(p + 8);
        size_t eq = rest.find('=');
        if (eq != std::string::npos) {
            rest = trimWsText(rest.substr(eq + 1));
            if (!rest.empty() && rest[0] == '"') {
                size_t q = rest.find('"', 1);
                if (q != std::string::npos) rest = rest.substr(1, q - 1);
            } else {
                size_t semi = rest.find(';');
                if (semi != std::string::npos) rest = rest.substr(0, semi);
            }
            rest = trimWsText(rest);
            if (!rest.empty()) { boundary = rest; return true; }
        }
    }
    // 2) 退而求其次：正文第一个非空行如果是 --xxx，xxx 就是 boundary
    std::istringstream iss(bodyText);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string t = trimWsText(line);
        if (t.empty()) continue;
        if (t.compare(0, 2, "--") == 0 && t.size() > 2) {
            boundary = t.substr(2);
            return true;
        }
        return false;
    }
    return false;
}

// 一封附件邮件的信息（只放"真正的附件"，即 Content-Disposition: attachment）
struct MimeAttachment {
    std::string contentType;  // 如 application/octet-stream
    std::string filename;     // 附件名
    std::string encoding;     // base64 / 8bit...
    std::string content;      // 未解码的内容
};

// 解析邮件原文，把 multipart 里的附件全部提取出来
bool parseAttachments(const std::string& mailText, std::vector<MimeAttachment>& atts) {
    atts.clear();
    std::string txt = toLfText(mailText);
    // 拆 头部区 / 正文区
    size_t blank = txt.find("\n\n");
    std::string headerText = txt;
    std::string bodyText;
    if (blank != std::string::npos) {
        headerText = txt.substr(0, blank);
        bodyText   = txt.substr(blank + 2);
    }

    std::string boundary;
    if (!findBoundary(headerText, bodyText, boundary)) return false;
    const std::string marker = "--" + boundary;

    // 按 boundary 行切段，收集每一段（part）
    std::vector<std::string> segments;
    std::string cur;
    bool inPart = false;
    {
        std::istringstream iss(bodyText);
        std::string line;
        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::string t = trimWsText(line);
            bool isMarker = (t == marker);
            bool isEnd    = (t == marker + "--");
            if (isMarker || isEnd) {
                if (inPart && !cur.empty()) segments.push_back(cur);
                cur.clear();
                inPart = true;
                if (isEnd) break;
            } else if (inPart) {
                cur += line + "\n";
            }
        }
    }
    // 兜底：最后一段若没被 flush 就补上
    if (inPart && !cur.empty()) segments.push_back(cur);

    for (std::string& seg : segments) {
        std::string segLf = seg;
        size_t b2 = segLf.find("\n\n");
        std::string partHead = segLf;
        std::string partBody;
        if (b2 != std::string::npos) {
            partHead = segLf.substr(0, b2);
            partBody = segLf.substr(b2 + 2);
        }
        std::string disp = partHeaderValue(partHead, "Content-Disposition");
        if (disp.find("attachment") == std::string::npos) continue;  // 只收真正的附件

        MimeAttachment a;
        a.contentType = partHeaderValue(partHead, "Content-Type");
        if (a.contentType.empty()) a.contentType = "application/octet-stream";
        a.filename    = parseFilename(disp);
        a.encoding    = partHeaderValue(partHead, "Content-Transfer-Encoding");
        a.content     = partBody;
        if (a.filename.empty()) a.filename = "attachment.bin";
        atts.push_back(a);
    }
    return true;
}

// 生成 multipart/mixed 正文区（文本段 + 附件段）
std::string makeMultipartText(const std::string& boundary,
                              const std::string& textBody,
                              const std::string& filename,
                              const std::string& fileB64) {
    std::string out;
    out += "--" + boundary + "\r\n";
    out += "Content-Type: text/plain; charset=utf-8\r\n";
    out += "Content-Transfer-Encoding: 8bit\r\n\r\n";
    out += textBody;
    if (textBody.empty() || textBody.back() != '\n') out += "\r\n";
    out += "--" + boundary + "\r\n";
    out += "Content-Type: application/octet-stream\r\n";
    out += "Content-Disposition: attachment; filename=\"" + filename + "\"\r\n";
    out += "Content-Transfer-Encoding: base64\r\n\r\n";
    out += fileB64;
    if (fileB64.empty() || fileB64.back() != '\n') out += "\r\n";
    out += "--" + boundary + "--\r\n";
    return out;
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
    } else if (req.method == "GET" && req.path == "/api/benchmark") {
        handleBenchmark(req, resp);
    } else if (req.method == "GET" && req.path == "/api/attachment") {
        handleAttachment(req, resp);
    } else if (req.method == "GET" && req.path == "/api/webkey") {
        handleWebKey(req, resp);                     // Web↔服务器 RSA：下发服务器公钥
    } else if (req.method == "POST" && req.path == "/api/webpub") {
        handleWebPub(req, resp);                     // Web↔服务器 RSA：登记浏览器公钥
    } else if (req.method == "POST" && req.path == "/api/raw/smtp/open") {
        handleRawOpen("smtp", req, resp);
    } else if (req.method == "POST" && req.path == "/api/raw/smtp/send") {
        handleRawSend("smtp", req, resp);
    } else if (req.method == "POST" && req.path == "/api/raw/smtp/close") {
        handleRawClose("smtp", req, resp);
    } else if (req.method == "POST" && req.path == "/api/raw/pop3/open") {
        handleRawOpen("pop3", req, resp);
    } else if (req.method == "POST" && req.path == "/api/raw/pop3/send") {
        handleRawSend("pop3", req, resp);
    } else if (req.method == "POST" && req.path == "/api/raw/pop3/close") {
        handleRawClose("pop3", req, resp);
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

    // ===================== 【第一层】Web↔服务器 RSA 信封解包 =====================
    // 浏览器把整个发送表单（to/subject/body/附件/algo…）加密成 RSA 信封放进 env 字段；
    // 这里用服务器 RSA 私钥拆封，解出的字段合并进 req.form，
    // 后续业务逻辑（含第二层 AES/ChaCha 对称加密→SMTP）完全复用、无需改动。
    const std::string webEnv = getParam(req, "env");
    if (!webEnv.empty()) {
        std::string inner;
        if (!MailCrypto::webEnvelopeOpen(webEnv, inner)) {
            resp.body = jsonResult(false,
                "Web RSA 信封解密失败（服务器密钥不匹配或内容被篡改）");
            return;
        }
        HttpRequest& mutReq = const_cast<HttpRequest&>(req);
        parseKeyValues(inner, mutReq.form);   // parseKeyValues 是合并语义（不清空）
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
    std::string algoName = getParam(req, "algo");   // "aes" / "chacha"，可缺省
    if (!wantEncrypt && (algoName == "aes" || algoName == "chacha")) {
        wantEncrypt = true;
    }
    if (wantEncrypt && algoName != "aes" && algoName != "chacha") {
        resp.body = jsonResult(false, "未知加密算法（仅支持 aes / chacha）");
        return;
    }

    // ---- 附件参数（可选）：filename = 文件名，fileB64 = 文件内容的 Base64 ----
    std::string filename = getParam(req, "filename");
    std::string fileB64  = getParam(req, "fileB64");
    bool hasAttach = !filename.empty() && !fileB64.empty();
    if (hasAttach && fileB64.size() > 2800000) {   // 单封约 ≤2.1MB（课程要求）的余量保护
        resp.body = jsonResult(false, "附件太大：单封邮件不能超过约 2.1MB");
        return;
    }

    // ---- 组装"正文区"：纯文本，或 multipart（文本段 + base64 附件段）----
    std::string boundary;
    std::string mimeHeaders;   // 有附件时附加的 MIME 头
    std::string payload;
    if (hasAttach) {
        boundary = "MailForgeBoundary" + std::to_string((long)time(nullptr))
                 + std::to_string(rand());
        payload = makeMultipartText(boundary, body, filename, fileB64);
        mimeHeaders = std::string("MIME-Version: 1.0\r\n")
                    + "Content-Type: multipart/mixed; boundary=\""
                    + boundary + "\"\r\n";
    } else {
        payload = body + "\r\n";
    }

    // ---- 拼一封标准邮件原文（头部区 + 空行 + 正文）----
    std::string mailSubject = subject;
    std::string cryptoHeader;   // 加密时额外写进头部区的标记行（便于识别/调试）
    if (wantEncrypt) {
        // ===================== 加密挂钩点①（发送前） =====================
        // 把"真实主题 + 整个正文区(含附件 multipart)"打包成载荷，
        // 用【收件人】的对称密钥按所选算法（AES-256-CBC / ChaCha20，自研）
        // 整体加密后再交给 SMTP 传输 → SMTP/POP3 链路上与 .eml 落盘都是密文，
        // 收件人阅读时（decodeMail）用自己账号的密钥还原。
        // 头部只留 Subject: [加密邮件] 占位与 X-MailForge-Crypto 标记。
        if (algoName.empty()) algoName = "aes";
        mailSubject = "[加密邮件]";
        std::string inner = "Subject: " + subject + "\r\n\r\n" + payload;
        std::string symKey;
        if (!MailCrypto::getUserKey(to, symKey)) {
            resp.body = jsonResult(false,
                "加密失败：无法生成/读取收件人密钥，请检查 keys/ 目录");
            return;
        }
        const MailCrypto::CryptoAlgo algo = (algoName == "chacha")
            ? MailCrypto::ALGO_CHACHA20 : MailCrypto::ALGO_AES_CBC;
        std::string cipher = MailCrypto::encryptPayload(inner, symKey, algo);
        if (cipher.empty()) {
            resp.body = jsonResult(false, "加密失败：加密算法内部出错");
            return;
        }
        payload = cipher + "\r\n";
        mimeHeaders.clear();                          // 密文不再是 multipart，不挂 MIME 头
        cryptoHeader = "X-MailForge-Crypto: " + algoName + "\r\n";
    }

    std::string plain =
        "From: " + from + "\r\n"
        "To: " + to + "\r\n"
        "Subject: " + mailSubject + "\r\n"
        + mimeHeaders
        + cryptoHeader
        + "\r\n" + payload;

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
        // decodeMail 会自动识别 AES/ChaCha20 加密邮件，并用收件人（session.user）
        // 账号的对称密钥解密还原主题/正文（见匿名命名空间实现）
        DecodedMail decoded = decodeMail(raw, session.user);
        std::string subject = decoded.subject;
        std::string from    = decoded.from;
        bool encrypted      = decoded.encrypted;
        bool hasAttach = decoded.display.find("Content-Disposition: attachment")
                       != std::string::npos;

        if (i > 0) json += ",";
        json += "{\"number\":" + std::to_string(mails[i].number)
              + ",\"size\":" + std::to_string(mails[i].size)
              + ",\"subject\":\"" + jsonEscape(subject)
              + "\",\"from\":\"" + jsonEscape(from)
              + "\",\"encrypted\":" + (encrypted ? "true" : "false")
              + ",\"attachment\":" + (hasAttach ? "true" : "false") + "}";
    }
    json += "]}";

    pop3.quit();   // 注意：没 DELE 任何邮件，服务器上的信不会丢（网页收信≠删信）
    // 【第一层】若浏览器登记过 RSA 公钥且带 web=1，整个收件箱 JSON 用信封返回
    if (sealWebResponseIfNeeded(req, session, json, resp)) return;
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
    DecodedMail decoded = decodeMail(raw, session.user);

    // 顺带解析附件列表（multipart 里的 Content-Disposition: attachment 段）
    std::vector<MimeAttachment> atts;
    parseAttachments(decoded.display, atts);
    std::string attJson = "\"attachments\":[";
    for (size_t k = 0; k < atts.size(); ++k) {
        if (k > 0) attJson += ",";
        attJson += "{\"i\":" + std::to_string(k)
                 + ",\"filename\":\"" + jsonEscape(atts[k].filename)
                 + "\",\"type\":\"" + jsonEscape(atts[k].contentType) + "\"}";
    }
    attJson += "]";

    const std::string plainBody =
        std::string("{\"ok\":true,\"number\":") + std::to_string(number)
        + ",\"encrypted\":" + (decoded.encrypted ? "true" : "false")
        + ",\"raw\":\"" + jsonEscape(decoded.display) + "\","
        + attJson + "}";
    // 【第一层】浏览器登记过 RSA 公钥且带 web=1 → 明文 raw 用信封封装返回
    if (sealWebResponseIfNeeded(req, session, plainBody, resp)) return;
    resp.body = plainBody;
}

// ==================== GET /api/attachment ====================
// 参数：token, n（邮件编号）, i（附件下标，见 /api/mail 的 attachments）
// 返回：附件二进制内容（带 Content-Disposition 下载头）
void HttpServer::handleAttachment(const HttpRequest& req, HttpResponse& resp) {
    Session session;
    if (!loginAndGetSession(getParam(req, "token"), session)) {
        resp.body = jsonResult(false, "token 无效或已过期，请先登录");
        return;
    }
    int number = atoi(getParam(req, "n").c_str());
    int index  = atoi(getParam(req, "i").c_str());
    if (number <= 0 || index < 0) {
        resp.body = jsonResult(false, "缺少合法的 n / i 参数");
        return;
    }

    Pop3Client pop3(kMailServerIp, kPop3Port);
    if (!pop3.login(session.user, session.pass)) {
        resp.body = jsonResult(false, "POP3 登录失败: " + pop3.getLastError());
        return;
    }
    std::string raw;
    bool retrOk = pop3.retr(number, raw);
    pop3.quit();
    if (!retrOk) {
        resp.body = jsonResult(false, "读取第 " + std::to_string(number) + " 封失败");
        return;
    }

    DecodedMail decoded = decodeMail(raw, session.user);
    std::vector<MimeAttachment> atts;
    parseAttachments(decoded.display, atts);
    if (index >= (int)atts.size()) {
        resp.body = jsonResult(false, "该邮件没有这个附件下标");
        return;
    }
    const MimeAttachment& a = atts[index];

    // 按编码解码：base64 段还原成原始字节，其它原样返回
    std::string data;
    std::string enc;
    for (char c : a.encoding) enc += (char)tolower((unsigned char)c);
    if (enc.find("base64") != std::string::npos) {
        data = MailCrypto::base64Decode(a.content);
    } else {
        data = a.content;
    }

    // 附件名消毒（去掉引号/换行，防止注入响应头）
    std::string fn = a.filename;
    for (char& c : fn) {
        if (c == '"' || c == '\r' || c == '\n' || c == ';') c = '_';
    }

    resp.status      = 200;
    resp.statusText  = "OK";
    resp.contentType = a.contentType.empty() ? "application/octet-stream" : a.contentType;
    resp.extraHeaders = "Content-Disposition: attachment; filename=\"" + fn + "\"\r\n";
    // 【第一层】web=1 时附件二进制也走 RSA 信封（信封内是 Base64 编码的附件数据）
    if (getParam(req, "web") == "1" && !session.webPubPem.empty()) {
        std::string b64 = MailCrypto::base64Encode(data);
        if (sealWebResponseIfNeeded(req, session, b64, resp)) return;
    }
    resp.body = data;
}

// ==================== Web↔服务器 RSA 密钥交换（第一层加密） ====================

// GET /api/webkey → 下发服务器 RSA 公钥（浏览器用它加密"发给服务器"的邮件表单）
void HttpServer::handleWebKey(const HttpRequest& req, HttpResponse& resp) {
    (void)req;   // 下发公钥无需登录态（公钥本身公开）
    resp.contentType = "application/json; charset=utf-8";
    std::string pem;
    if (!MailCrypto::ensureWebRsaKeys() ||
        !MailCrypto::webServerPublicKeyPem(pem)) {
        resp.body = jsonResult(false,
            "服务器 RSA 密钥初始化失败（检查 keys/ 目录权限与 OpenSSL 链接）");
        return;
    }
    resp.body = std::string("{\"ok\":true,\"pem\":\"") + jsonEscape(pem) + "\"}";
}

// POST /api/webpub 参数：token, pem —— 浏览器上传自己的 RSA-2048 公钥。
// 之后服务器对 /api/inbox、/api/mail、/api/attachment 的 web=1 响应做信封封装。
void HttpServer::handleWebPub(const HttpRequest& req, HttpResponse& resp) {
    resp.contentType = "application/json; charset=utf-8";
    const std::string token = getParam(req, "token");
    std::string pem = getParam(req, "pem");
    if (token.empty() || pem.empty() ||
        pem.find("BEGIN PUBLIC KEY") == std::string::npos) {
        resp.body = jsonResult(false, "参数不合法：需要 token 与 PKCS#8 PEM 公钥");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        auto it = sessions_.find(token);
        if (it == sessions_.end()) {
            resp.body = jsonResult(false, "token 无效或已过期");
            return;
        }
        it->second.webPubPem = pem;
    }
    resp.body = jsonResult(true, "Web RSA 公钥已登记：读信响应将加密返回");
}

// 若该会话登记过浏览器 RSA 公钥且本次请求带 web=1：
//   把明文响应封装成 RSA 信封，输出 {"ok":true,"cipher":"k=..|iv=..|ct=.."}，
//   返回 true；否则不动 resp、返回 false（按原明文逻辑继续）。
bool HttpServer::sealWebResponseIfNeeded(const HttpRequest& req, Session& session,
                                         const std::string& plainResp,
                                         HttpResponse& resp) {
    if (getParam(req, "web") != "1" || session.webPubPem.empty()) return false;
    std::string packed;
    if (!MailCrypto::webEnvelopeSeal(plainResp, session.webPubPem, packed)) {
        std::cerr << "[HTTP] Web RSA 信封封装失败，降级为明文返回" << std::endl;
        return false;
    }
    resp.status      = 200;
    resp.statusText  = "OK";
    resp.contentType = "application/json; charset=utf-8";
    resp.extraHeaders.clear();
    resp.body = std::string("{\"ok\":true,\"cipher\":\"") + jsonEscape(packed) + "\"}";
    return true;
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


// ==================== GET /api/benchmark ====================
// 性能压测：连发 100 封带约 1MB 附件(multipart)的邮件，统计成功率/丢包率/时延，
// 测完自动清理测试邮件。支持明文 / AES-256-CBC 加密 / ChaCha20 加密 / 三种全跑。
// 参数：token；encrypt = plain | aes | chacha | all（缺省 plain）
void HttpServer::handleBenchmark(const HttpRequest& req, HttpResponse& resp) {
    Session session;
    if (!loginAndGetSession(getParam(req, "token"), session)) {
        resp.body = jsonResult(false, "token 无效或已过期，请先登录");
        return;
    }

    const int kCount = 100;              // 每种模式连续发送 100 封
    const size_t kFileBytes = 786432;    // 附件"原始文件"786KB → base64 后 >1MB

    // 选择测试模式
    std::string modeParam = getParam(req, "encrypt");
    if (modeParam.empty() || modeParam == "0" || modeParam == "none") modeParam = "plain";
    if (modeParam == "1") modeParam = "aes";   // 兼容旧参数
    std::vector<std::string> modes;
    if (modeParam == "all" || modeParam == "both") {
        modes = {"plain", "aes", "chacha"};
    } else if (modeParam == "plain" || modeParam == "aes" || modeParam == "chacha") {
        modes = {modeParam};
    } else {
        resp.body = jsonResult(false, "encrypt 参数只能是 plain / aes / chacha / all");
        return;
    }

    std::string to   = session.user + "@example.com";
    std::string from = to;

    auto nowMs = []() -> long long {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    };

    // 预检：账号能登录 POP3（后续每轮统计也依赖它）
    {
        Pop3Client pre(kMailServerIp, kPop3Port);
        if (!pre.login(session.user, session.pass)) {
            resp.body = jsonResult(false, "压测前登录失败，请确认账号密码正确");
            return;
        }
        pre.quit();
    }

    // 构造约 1MB 的"附件"邮件：786KB 文件 → base64 → MIME multipart
    std::string fileContent(kFileBytes, 'Z');
    std::string fileB64 = MailCrypto::base64Encode(fileContent);
    std::string boundary = "MailForgeBench" + std::to_string((long)time(nullptr))
                         + std::to_string(rand());
    std::string mimeHeaders = std::string("MIME-Version: 1.0\r\n")
                            + "Content-Type: multipart/mixed; boundary=\""
                            + boundary + "\"\r\n";
    std::string payload = makeMultipartText(boundary, "压测正文（带附件）",
                                            "[压测附件].bin", fileB64);
    const size_t mailBytes = payload.size() + 400;   // 单封落盘估算，>1MB

    // 单轮压测：mode = plain / aes / chacha。返回该轮 JSON 片段并打印日志。
    auto runRound = [&](const std::string& mode) -> std::string {
        const bool isEnc = (mode != "plain");
        MailCrypto::CryptoAlgo algo =
            (mode == "chacha") ? MailCrypto::ALGO_CHACHA20
            : (mode == "aes")  ? MailCrypto::ALGO_AES_CBC
            : MailCrypto::ALGO_NONE;

        // 加密轮：准备本账号对称密钥（发给自己，收发用同一把）
        std::string symKey;
        const bool keyReady = !isEnc || MailCrypto::getUserKey(to, symKey);

        // a) 压测前邮箱基准
        int baseCount = 0;
        long long baseBytes = 0;
        {
            Pop3Client cnt(kMailServerIp, kPop3Port);
            if (!cnt.login(session.user, session.pass)) return "";
            cnt.stat(baseCount, baseBytes);
            cnt.quit();
        }

        // b) 连续发送 kCount 封并逐封计时
        int smtpOk = 0, smtpFail = 0;
        long long totalLatencyMs = 0;
        const long long roundStart = nowMs();
        std::cout << "[HTTP] 压测轮 " << mode << "：" << session.user << " 连发 "
                  << kCount << " 封约 " << (mailBytes / 1024) << "KB 邮件..."
                  << std::endl;

        for (int i = 0; i < kCount; ++i) {
            long long t0 = nowMs();
            std::string subject = "[压测] 第 " + std::to_string(i + 1)
                                + "/" + std::to_string(kCount) + " 封";
            std::string raw;
            if (!isEnc) {
                raw = "From: " + from + "\r\n"
                      "To: " + to + "\r\n"
                      "Subject: " + subject + "\r\n"
                      + mimeHeaders + "\r\n" + payload;
            } else {
                // 加密轮：真实主题+正文(含附件)整体加密后再进 SMTP，落盘只有密文
                std::string inner = "Subject: " + subject + "\r\n\r\n" + payload;
                std::string cipher =
                    keyReady ? MailCrypto::encryptPayload(inner, symKey, algo) : "";
                if (cipher.empty()) {
                    ++smtpFail;
                    std::cerr << "[HTTP] 第" << (i + 1) << "封加密失败("
                              << mode << ")" << std::endl;
                    continue;
                }
                raw = "From: " + from + "\r\n"
                      "To: " + to + "\r\n"
                      "Subject: [加密邮件]\r\n"
                      "X-MailForge-Crypto: " + mode + "\r\n"
                      "\r\n" + cipher + "\r\n";
            }

            SmtpClient smtp(kMailServerIp, kSmtpPort);
            const bool ok = smtp.sendRawMail(from, to, raw);
            const long long el = nowMs() - t0;
            if (ok) { ++smtpOk; totalLatencyMs += el; }
            else {
                ++smtpFail;
                std::cerr << "[HTTP] 第" << (i + 1) << "封发送失败: "
                          << smtp.getLastError() << std::endl;
            }
            if (i % 10 == 9) usleep(200 * 1000);
        }

        // c) 收件端统计
        int afterCount = 0, received = 0;
        long long afterBytes = 0;
        {
            Pop3Client cnt(kMailServerIp, kPop3Port);
            if (cnt.login(session.user, session.pass) &&
                cnt.stat(afterCount, afterBytes)) {
                received = afterCount - baseCount;
                if (received < 0) received = 0;
            }
            cnt.quit();
        }

        // d) 加密轮：逐封 RETR 并解密，验证"密文可被正确还原"
        int decryptOk = 0;
        if (isEnc && received > 0) {
            Pop3Client ver(kMailServerIp, kPop3Port);
            if (ver.login(session.user, session.pass)) {
                std::vector<Pop3MailInfo> list;
                if (ver.list(list)) {
                    int n = 0;
                    for (size_t k = list.size(); k > 0 && n < received; --k) {
                        std::string raw;
                        if (!ver.retr(list[k - 1].number, raw)) continue;
                        const DecodedMail dm = decodeMail(raw, session.user);
                        if (dm.display.find("[压测附件].bin") != std::string::npos)
                            ++decryptOk;
                        ++n;
                    }
                }
                ver.quit();
            }
        }

        // e) 清理本轮测试邮件
        if (received > 0) {
            Pop3Client cleaner(kMailServerIp, kPop3Port);
            if (cleaner.login(session.user, session.pass)) {
                std::vector<Pop3MailInfo> list;
                if (cleaner.list(list)) {
                    int deleted = 0;
                    for (size_t k = list.size(); k > 0 && deleted < received; --k) {
                        if (cleaner.dele(list[k - 1].number)) ++deleted;
                    }
                }
                cleaner.quit();
            }
        }

        // f) 结果统计
        const int lost = kCount - received;
        const double lossRate = (lost * 100.0) / kCount;
        const double avgLatencyMs =
            smtpOk > 0 ? (double)totalLatencyMs / smtpOk : 0.0;
        std::cout << "[HTTP] 压测轮 " << mode << " 结束：发送成功 " << smtpOk
                  << "/" << kCount << "，实际收到 " << received
                  << "，丢包率 " << lossRate << "%"
                  << (isEnc ? "，解密还原 " + std::to_string(decryptOk) : "")
                  << std::endl;

        return std::string("{")
            + "\"mode\":\"" + mode + "\""
            + ",\"encrypted\":" + (isEnc ? "true" : "false")
            + ",\"count\":" + std::to_string(kCount)
            + ",\"smtpOk\":" + std::to_string(smtpOk)
            + ",\"smtpFail\":" + std::to_string(smtpFail)
            + ",\"received\":" + std::to_string(received)
            + ",\"decryptOk\":" + std::to_string(decryptOk)
            + ",\"lost\":" + std::to_string(lost)
            + ",\"lossRate\":" + std::to_string(lossRate)
            + ",\"avgLatencyMs\":" + std::to_string(avgLatencyMs)
            + ",\"totalMs\":" + std::to_string(nowMs() - roundStart)
            + "}";
    };

    // 3) 依次跑完各模式
    std::string resultsJson;
    long long startMs = nowMs();
    for (size_t m = 0; m < modes.size(); ++m) {
        const std::string rj = runRound(modes[m]);
        if (rj.empty()) {
            resp.body = jsonResult(false, "压测中断：POP3 会话失败");
            return;
        }
        if (!resultsJson.empty()) resultsJson += ",";
        resultsJson += rj;
    }

    // 汇总（用于 JSON 顶层与"全部模式"的概览）
    const std::string plainBody =
        std::string("{\"ok\":true")
        + ",\"modes\":[" + resultsJson + "]"
        + ",\"sizeBytes\":" + std::to_string(mailBytes)
        + ",\"count\":\"" + modeParam + "\""
        + ",\"totalMs\":" + std::to_string(nowMs() - startMs)
        + ",\"msg\":\"压测完成，测试邮件已自动清理\"}";
    resp.contentType = "application/json; charset=utf-8";
    // 压测与发信一致：请求带 web=1 且登记过浏览器公钥时，结果以 RSA 信封返回
    if (sealWebResponseIfNeeded(req, session, plainBody, resp)) return;
    resp.body = plainBody;
}


// ==================== 交互式协议终端（一问一答，保持连接） ====================
namespace {
// 一条正在进行的协议会话：fd 是连着 2525/1110 的 socket
struct RawConn { int fd = -1; long long lastMs = 0; };
std::map<std::string, RawConn> gRawConns;
std::mutex gRawMutex;
const long long kConnTimeoutMs = 300000;   // 5 分钟没操作就自动断开

long long rawNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

void rawSetTmo(int fd, int ms) {
    struct timeval tv; tv.tv_sec = ms / 1000; tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

int rawConnect(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port   = htons(port);
    inet_pton(AF_INET, kMailServerIp, &a.sin_addr);
    if (connect(fd, (struct sockaddr*)&a, sizeof(a)) < 0) { close(fd); return -1; }
    rawSetTmo(fd, 700);
    return fd;
}

bool rawSendLine(int fd, const std::string& line) {
    std::string msg = line + "\r\n";
    size_t t = 0;
    while (t < msg.size()) {
        ssize_t n = send(fd, msg.data() + t, msg.size() - t, 0);
        if (n < 0) return false;
        t += (size_t)n;
    }
    return true;
}

// 读一行；timeout=true 表示“没等到数据”（DATA 正文阶段服务器本来就不答话）
bool rawReadLine(int fd, std::string& line, bool& timeout) {
    line.clear(); timeout = false;
    char c;
    while (true) {
        ssize_t n = recv(fd, &c, 1, 0);
        if (n == 0) return false;                     // 服务器关闭连接
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) { timeout = true; return false; }
            return false;
        }
        if (c == '\n') break;
        if (line.size() < 16000) line += c;
    }
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return true;
}

// 找会话；超时则删除并报错
int rawGetConn(const std::string& key, std::string& err) {
    std::lock_guard<std::mutex> lock(gRawMutex);
    auto it = gRawConns.find(key);
    if (it == gRawConns.end()) { err = "会话不存在或已断开，请先点\"连接\""; return -1; }
    if (rawNowMs() - it->second.lastMs > kConnTimeoutMs) {
        close(it->second.fd);
        gRawConns.erase(it);
        err = "会话超时已自动断开，请重新连接";
        return -1;
    }
    it->second.lastMs = rawNowMs();
    return it->second.fd;
}

void rawStoreConn(const std::string& key, int fd) {
    std::lock_guard<std::mutex> lock(gRawMutex);
    auto it = gRawConns.find(key);
    if (it != gRawConns.end()) close(it->second.fd);   // 覆盖旧会话
    RawConn c; c.fd = fd; c.lastMs = rawNowMs();
    gRawConns[key] = c;
}

void rawDropConn(const std::string& key) {
    std::lock_guard<std::mutex> lock(gRawMutex);
    auto it = gRawConns.find(key);
    if (it != gRawConns.end()) { close(it->second.fd); gRawConns.erase(it); }
}
} // namespace

// ==================== POST /api/raw/<smtp|pop3>/open ====================
// 建立一条到协议端口的真实长连接，返回连接 id 与服务器问候
void HttpServer::handleRawOpen(const std::string& proto,
                               const HttpRequest& req, HttpResponse& resp) {
    resp.contentType = "application/json; charset=utf-8";
    Session session;
    if (!loginAndGetSession(getParam(req, "token"), session)) {
        resp.body = jsonResult(false, "token 无效或已过期，请先登录");
        return;
    }
    int port = (proto == "smtp") ? kSmtpPort : kPop3Port;
    int fd = rawConnect(port);
    if (fd < 0) { resp.body = jsonResult(false, "连接 " + proto + "(端口 "
                         + std::to_string(port) + ") 失败"); return; }

    // 读问候（等最长 3 秒）
    rawSetTmo(fd, 3000);
    std::string line; bool tmo;
    std::string linesJson;
    if (rawReadLine(fd, line, tmo)) {
        linesJson = "{\"who\":\"S\",\"text\":\"" + jsonEscape(line) + "\"}";
    } else if (!tmo) {
        close(fd);
        resp.body = jsonResult(false, "服务器没回问候就断开了");
        return;
    }
    rawSetTmo(fd, 700);

    std::string conn = proto + ":" + std::to_string(rawNowMs())
                     + ":" + std::to_string(rand());
    rawStoreConn(conn, fd);

    resp.body = std::string("{\"ok\":true,\"conn\":\"") + conn
              + "\",\"lines\":[" + linesJson + "]}";
}

// ==================== POST /api/raw/<smtp|pop3>/send ====================
// 把你输入的那一行原样发给服务器，并把服务器回的应答带回页面
void HttpServer::handleRawSend(const std::string& proto,
                               const HttpRequest& req, HttpResponse& resp) {
    resp.contentType = "application/json; charset=utf-8";
    Session session;
    if (!loginAndGetSession(getParam(req, "token"), session)) {
        resp.body = jsonResult(false, "token 无效或已过期，请先登录");
        return;
    }
    std::string conn = getParam(req, "conn");
    std::string line = getParam(req, "line");
    if (conn.empty() || line.size() > 16000) {
        resp.body = jsonResult(false, "参数不对");
        return;
    }
    // 行尾可能带 \r 或空格，统一去掉行尾 \r（命令以 \n 结尾即可）
    while (!line.empty() && (line.back() == '\r')) line.pop_back();

    std::string err;
    int fd = rawGetConn(conn, err);
    if (fd < 0) { resp.body = jsonResult(false, err); return; }

    if (!rawSendLine(fd, line)) {
        rawDropConn(conn);
        resp.body = jsonResult(false, "发送失败，连接可能已断开");
        return;
    }

    // 读取应答：等 700ms；DATA 正文阶段服务器不答话 → 正常（waiting=true）
    std::string reply;
    bool tmo = false;
    std::string jsonLines;
    bool closed = false;

    bool got = rawReadLine(fd, reply, tmo);
    if (got) {
        jsonLines += "{\"who\":\"S\",\"text\":\"" + jsonEscape(reply) + "\"}";
    } else if (!tmo) {
        closed = true;
        rawDropConn(conn);
    }

    // 多行应答 / 收尾处理
    if (got) {
        // 该命令的基础名（转大写，用于判断多行应答）
        std::string base = line;
        size_t sp = line.find(' ');
        if (sp != std::string::npos) base = line.substr(0, sp);
        for (auto& c : base) c = (char)toupper((unsigned char)c);

        bool multi = false;
        if (proto == "pop3") {
            multi = (base == "RETR" || base == "TOP");
            if (base == "LIST" && line.find(' ') == std::string::npos) multi = true;
        } else {   // smtp：'250-xxx' 形式的多行扩展应答
            multi = (reply.size() >= 4 && reply[3] == '-');
            if (base == "DATA" && reply.compare(0, 3, "354") == 0) multi = false;
        }
        if (multi) {
            rawSetTmo(fd, 400);
            while (true) {
                std::string more; bool tm2 = false;
                if (!rawReadLine(fd, more, tm2)) {
                    if (!tm2) { closed = true; rawDropConn(conn); }
                    break;
                }
                if (!jsonLines.empty()) jsonLines += ",";
                jsonLines += "{\"who\":\"S\",\"text\":\"" + jsonEscape(more) + "\"}";
                if (more == ".") break;          // POP3 多行内容结束
                if (proto == "smtp" && more.size() >= 4 && more[3] != '-') break;
                if (proto == "smtp" && more.size() < 4) break;
            }
            rawSetTmo(fd, 700);
        }

        // QUIT 之后服务器会关连接，主动读一次确认
        if (base == "QUIT") {
            rawSetTmo(fd, 400);
            std::string last; bool tm3 = false;
            if (!rawReadLine(fd, last, tm3) && !tm3) { closed = true; rawDropConn(conn); }
        }
    }

    resp.body = std::string("{\"ok\":true,\"waiting\":") + (got ? "false" : "true")
              + ",\"closed\":" + (closed ? "true" : "false")
              + ",\"lines\":[" + jsonLines + "]}";
}

// ==================== POST /api/raw/<smtp|pop3>/close ====================
void HttpServer::handleRawClose(const std::string& proto,
                                const HttpRequest& req, HttpResponse& resp) {
    (void)proto;   // 断开时协议已包含在 conn 里
    resp.contentType = "application/json; charset=utf-8";
    Session session;
    if (!loginAndGetSession(getParam(req, "token"), session)) {
        resp.body = jsonResult(false, "token 无效或已过期");
        return;
    }
    rawDropConn(getParam(req, "conn"));
    resp.body = jsonResult(true, "已断开");
}
