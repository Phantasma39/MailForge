// 用于实现 POP3 客户端的代码部分
// 思路和服务端 Pop3Server.cpp 对称，可以对照着看

#include "Pop3Client.h"
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/time.h>
#include <sstream>

namespace {
const int kTimeoutSec = 5;          // 收发超时：5 秒
const size_t kMaxLineLen = 8192;    // 一行响应最大长度（防御异常服务器）
}

// 默认连本机 POP3 端口 1110
Pop3Client::Pop3Client() : Pop3Client("127.0.0.1", 1110) {}

// 指定服务器与端口
Pop3Client::Pop3Client(const std::string& server, int port)
    : sockFd_(-1), server_(server), port_(port) {}

Pop3Client::~Pop3Client() {
    close();   // 析构时关 socket（注意：没发 QUIT 的话，DELE 标记不会生效）
}

// ==================== 底层收发（和 SmtpClient 几乎一样） ====================

// 建立 TCP 连接 + 设置 5 秒收发超时
bool Pop3Client::connectServer() {
    sockFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sockFd_ < 0) {
        lastError_ = "socket 创建失败: " + std::string(strerror(errno));
        return false;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);

    // 支持 127.0.0.1 和 localhost 两种写法
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

    struct timeval tv;
    tv.tv_sec = kTimeoutSec;
    tv.tv_usec = 0;
    setsockopt(sockFd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sockFd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return true;
}

// 发送一行命令（补 \r\n）
bool Pop3Client::sendLine(const std::string& line) {
    if (sockFd_ < 0) {
        lastError_ = "尚未连接服务器";
        return false;
    }
    std::string data = line + "\r\n";
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = send(sockFd_, data.data() + sent, data.size() - sent, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            lastError_ = "send 失败: " + std::string(strerror(errno));
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

// 读一行响应（到 \n 为止），去掉行尾 \r\n
bool Pop3Client::recvLine(std::string& line) {
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
        if (c == '\n') break;
        if (line.size() < kMaxLineLen) line += c;
    }
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return true;
}

// 读一行并判断是否 +OK；这行原文会存进 replyLine 供调用方继续解析
bool Pop3Client::readReply(std::string& replyLine) {
    if (!recvLine(replyLine)) return false;
    if (replyLine.rfind("+OK", 0) != 0) {
        lastError_ = "服务器返回错误: " + replyLine;
        return false;
    }
    return true;
}

// 读多行响应：从"下一行"开始一直读到单独一行的 "." 为止
// 期间自动做点填充还原（收到 ".." 开头 → 还原成 "." 开头）
bool Pop3Client::readMultiLines(std::vector<std::string>& lines) {
    lines.clear();
    while (true) {
        std::string line;
        if (!recvLine(line)) return false;
        if (line == ".") break;                      // 多行内容结束标记
        if (line.size() > 1 && line[0] == '.' && line[1] == '.') {
            line = line.substr(1);                    // 还原点填充 "..x" → ".x"
        }
        lines.push_back(line);
    }
    return true;
}

// 从 "+OK 2 516" 这样的响应里解析出两个数字
bool Pop3Client::parseTwoNumbers(const std::string& reply, long long& a, long long& b) {
    std::istringstream iss(reply);
    std::string tag;
    if (!(iss >> tag)) return false;      // 先读走 "+OK"
    if (!(iss >> a)) return false;        // 第一个数字
    if (!(iss >> b)) b = 0;               // 第二个数字可能没有，宽容处理
    return true;
}

// 关闭 socket
void Pop3Client::close() {
    if (sockFd_ != -1) {
        ::close(sockFd_);
        sockFd_ = -1;
    }
}

// ==================== 核心命令 ====================

// 登录：未连接则先建连，然后 USER + PASS
bool Pop3Client::login(const std::string& user, const std::string& pass) {
    lastError_.clear();
    if (sockFd_ == -1) {
        if (!connectServer()) return false;
        std::string greeting;
        if (!recvLine(greeting)) return false;   // 吃掉服务器的 +OK 问候
        if (greeting.rfind("+OK", 0) != 0) {
            lastError_ = "服务器问候异常: " + greeting;
            return false;
        }
    }

    // USER：提交用户名
    std::string reply;
    if (!sendLine("USER " + user)) return false;
    if (!readReply(reply)) return false;

    // PASS：提交密码（通过后进入"事务态"，可以收发命令）
    if (!sendLine("PASS " + pass)) return false;
    if (!readReply(reply)) return false;
    return true;
}

// STAT：返回 未删除邮件数 count 和总字节数 totalBytes
bool Pop3Client::stat(int& count, long long& totalBytes) {
    std::string reply;
    if (!sendLine("STAT")) return false;
    if (!readReply(reply)) return false;

    long long a = 0, b = 0;
    if (!parseTwoNumbers(reply, a, b)) {
        lastError_ = "STAT 响应解析失败: " + reply;
        return false;
    }
    count = (int)a;
    totalBytes = b;
    return true;
}

// LIST：拉取未删除邮件的 编号+大小 列表
bool Pop3Client::list(std::vector<Pop3MailInfo>& mails) {
    mails.clear();
    std::string reply;
    if (!sendLine("LIST")) return false;
    if (!readReply(reply)) return false;    // 首行：+OK N message(s) ...

    std::vector<std::string> lines;
    if (!readMultiLines(lines)) return false;   // 剩下的行到 "." 结束

    for (const std::string& line : lines) {
        std::istringstream iss(line);
        Pop3MailInfo info;
        if (iss >> info.number >> info.size) {
            mails.push_back(info);
        }
    }
    return true;
}

// RETR：下载第 number 封邮件的完整原文（自动还原点填充）
bool Pop3Client::retr(int number, std::string& rawMail) {
    rawMail.clear();
    std::string reply;
    if (!sendLine("RETR " + std::to_string(number))) return false;
    if (!readReply(reply)) return false;     // 首行：+OK N octets

    std::vector<std::string> lines;
    if (!readMultiLines(lines)) return false;

    // 把收到的行拼回一封完整邮件（用 \n 连接即可，方便阅读/显示）
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) rawMail += "\n";
        rawMail += lines[i];
    }
    return true;
}

// DELE：把第 number 封标记删除（只有 QUIT 服务器才真正删）
bool Pop3Client::dele(int number) {
    std::string reply;
    if (!sendLine("DELE " + std::to_string(number))) return false;
    return readReply(reply);
}

// RSET：撤销本次会话所有删除标记
bool Pop3Client::rset() {
    std::string reply;
    if (!sendLine("RSET")) return false;
    return readReply(reply);
}

// NOOP：保活
bool Pop3Client::noop() {
    std::string reply;
    if (!sendLine("NOOP")) return false;
    return readReply(reply);
}

// QUIT：正常退出。服务器此时会真正删除被 DELE 标记的邮件
bool Pop3Client::quit() {
    if (sockFd_ == -1) return true;   // 本来就没连，视为成功
    std::string reply;
    bool ok = sendLine("QUIT") && recvLine(reply);
    close();   // 不管结果如何都要断开
    if (!ok) return false;
    if (reply.rfind("+OK", 0) != 0) {
        lastError_ = "QUIT 失败: " + reply;
        return false;
    }
    return true;
}

