// 用于实现 SMTP 客户端的代码部分
// 思路和服务端 SmtpServer.cpp 对称，可以对照着看

#include "SmtpClient.h"
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/time.h>
#include <vector>

namespace {
const int kTimeoutSec = 5;          // 收发超时：5 秒
const size_t kMaxLineLen = 8192;    // 一行响应最大长度（防御异常服务器）
}

// 默认连本机 SMTP 端口 2525
SmtpClient::SmtpClient() : SmtpClient("127.0.0.1", 2525) {}

// 指定服务器与端口
SmtpClient::SmtpClient(const std::string& server, int port)
    : sockFd_(-1), server_(server), port_(port) {}

SmtpClient::~SmtpClient() {
    close();   // 析构时确保关闭 socket
}

// ==================== 底层收发 ====================

// 建立 TCP 连接，并设置收发超时（防止网络异常时 recv 永远阻塞）
bool SmtpClient::connectServer() {
    sockFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sockFd_ < 0) {
        lastError_ = "socket 创建失败: " + std::string(strerror(errno));
        return false;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);

    // 服务器地址支持两种写法：数字 IP（127.0.0.1）或主机名（localhost）
    if (inet_pton(AF_INET, server_.c_str(), &addr.sin_addr) != 1) {
        struct hostent* he = gethostbyname(server_.c_str());
        if (!he) {
            lastError_ = "无法解析服务器地址: " + server_;
            close();
            return false;
        }
        memcpy(&addr.sin_addr, he->h_addr, he->h_length);
    }

    if (connect(sockFd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        lastError_ = "连接服务器 " + server_ + ":" + std::to_string(port_)
                     + " 失败: " + strerror(errno);
        close();
        return false;
    }

    // 设置收发超时：超过 5 秒没收到数据就返回失败，避免程序卡死
    struct timeval tv;
    tv.tv_sec = kTimeoutSec;
    tv.tv_usec = 0;
    setsockopt(sockFd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sockFd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return true;
}

// 发送一行：内容 + \r\n（SMTP 规定每行必须以 CRLF 结尾）
bool SmtpClient::sendLine(const std::string& line) {
    if (sockFd_ < 0) {
        lastError_ = "尚未连接服务器";
        return false;
    }
    std::string data = line + "\r\n";
    size_t sent = 0;
    while (sent < data.size()) {   // 循环 send，保证全部发出去
        ssize_t n = send(sockFd_, data.data() + sent, data.size() - sent, 0);
        if (n < 0) {
            if (errno == EINTR) continue;   // 被信号打断，重发
            lastError_ = "send 失败: " + std::string(strerror(errno));
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

// 读一行响应（读到 \n 为止），去掉行尾的 \r\n
bool SmtpClient::recvLine(std::string& line) {
    line.clear();
    if (sockFd_ < 0) {
        lastError_ = "尚未连接服务器";
        return false;
    }
    char c;
    while (true) {
        ssize_t n = recv(sockFd_, &c, 1, 0);
        if (n == 0) {
            lastError_ = "连接被服务器关闭";
            return false;
        }
        if (n < 0) {
            lastError_ = "recv 失败: " + std::string(strerror(errno));
            return false;
        }
        if (c == '\n') break;                 // 一行结束
        if (line.size() < kMaxLineLen) line += c;   // 超过上限就丢弃（防御）
    }
    if (!line.empty() && line.back() == '\r') line.pop_back();   // 去掉 \r
    return true;
}

// 等待服务器回一个指定状态码（如 250）
// 服务器可能用 250-xxx 多行响应，需要一直读到 "250 xxx" 为止
bool SmtpClient::waitReply(int expectCode) {
    std::string line;
    if (!recvLine(line)) return false;
    if (line.size() < 3) {
        lastError_ = "收到异常响应: " + line;
        return false;
    }
    int code = std::stoi(line.substr(0, 3));
    // 多行响应：形如 "250-第一行" ... "250 最后一行"
    while (line.size() >= 4 && line[3] == '-') {
        if (!recvLine(line)) return false;
    }
    if (code != expectCode) {
        lastError_ = "期望响应码 " + std::to_string(expectCode)
                     + "，实际收到: " + line;
        return false;
    }
    return true;
}

// 关闭 socket
void SmtpClient::close() {
    if (sockFd_ != -1) {
        ::close(sockFd_);
        sockFd_ = -1;
    }
}

// ==================== 核心：发一封邮件 ====================

// sendMail = 先拼好"带头部"的完整邮件原文，再走一次标准 SMTP 会话
bool SmtpClient::sendMail(const std::string& from,
                          const std::string& to,
                          const std::string& subject,
                          const std::string& body) {
    // ---- 1. 拼 RFC 5322 邮件原文：头部区 + 空行 + 正文 ----
    std::string raw;
    raw += "From: " + from + "\r\n";
    raw += "To: " + to + "\r\n";
    raw += "Subject: " + subject + "\r\n";
    raw += "\r\n";                     // 空行：头部区结束的标志
    raw += body;
    if (!body.empty() && body.back() != '\n') raw += "\n";   // 保证正文以换行结尾

    // ---- 2. 交给 sendRawMail 走完整 SMTP 会话 ----
    return sendRawMail(from, to, raw);
}

// sendRawMail：走一次完整的 SMTP 发送会话，rawMail 原文整段发出
bool SmtpClient::sendRawMail(const std::string& from,
                             const std::string& to,
                             const std::string& rawMail) {
    lastError_.clear();

    // 1. 建连，等服务器 220 问候
    if (!connectServer()) return false;
    if (!waitReply(220)) { close(); return false; }

    // 2. EHLO 打招呼
    if (!sendLine("EHLO MailForgeClient")) { close(); return false; }
    if (!waitReply(250)) { close(); return false; }

    // 3. MAIL FROM 声明发件人
    if (!sendLine("MAIL FROM:<" + from + ">")) { close(); return false; }
    if (!waitReply(250)) { close(); return false; }

    // 4. RCPT TO 声明收件人
    if (!sendLine("RCPT TO:<" + to + ">")) { close(); return false; }
    if (!waitReply(250)) { close(); return false; }

    // 5. DATA 进入正文模式
    if (!sendLine("DATA")) { close(); return false; }
    if (!waitReply(354)) { close(); return false; }

    // 6. 把邮件原文逐行发出（自动点填充），以单独的 "." 结束
    if (!sendRawContent(rawMail)) { close(); return false; }

    // 7. 服务器确认接收
    if (!waitReply(250)) { close(); return false; }

    // 8. QUIT 礼貌退出（即使 QUIT 的应答没读到也无妨，信已经发出去了）
    sendLine("QUIT");
    waitReply(221);
    close();
    return true;
}

// 把一整段文本按行发出去：每行做 SMTP 点填充，最后发单独的 "." 表示结束
bool SmtpClient::sendRawContent(const std::string& rawMail) {
    // 按 \n 切行；顺带把 \r\n 里的 \r 去掉（下面统一补规范的 \r\n）
    std::vector<std::string> lines;
    std::string cur;
    for (char ch : rawMail) {
        if (ch == '\n') {
            lines.push_back(cur);
            cur.clear();
        } else if (ch != '\r') {
            cur += ch;
        }
    }
    if (!cur.empty()) lines.push_back(cur);
    // 如果文本以换行结尾，拆出来的最后一个空行是"多余的"，去掉
    if (!lines.empty() && lines.back().empty()
        && !rawMail.empty() && rawMail.back() == '\n') {
        lines.pop_back();
    }

    for (const std::string& line : lines) {
        std::string send = line;
        // SMTP 点填充：以 "." 开头的行要写成 ".."，否则服务器会误以为正文结束
        if (!send.empty() && send[0] == '.') send = "." + send;
        if (!sendLine(send)) return false;
    }

    return sendLine(".");   // 结束标记
}

