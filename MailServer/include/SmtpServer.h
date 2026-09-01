// ============================================================================
//  SmtpServer.h —— SMTP 服务器类（继承自 Server 基类）
//
//  作用：
//    实现 SMTP（Simple Mail Transfer Protocol，简单邮件传输协议）的服务器端。
//    SMTP 是互联网上"发邮件"的标准协议，本质上是一个基于 TCP 的文本协议：
//      客户端发一行文本命令（HELO、MAIL FROM、RCPT TO、DATA、QUIT...）
//      服务器回一行状态码（250、354、221...）
//
//  本类负责的部分（协议层）：
//    - 接收客户端的命令
//    - 解析命令并维护会话状态（状态机）
//    - 按 SMTP 规范返回响应码
//    - 把收到的邮件保存到本地文件
//
//  继承自 Server（网络层）：
//    - TCP 连接的建立、accept、多线程由 Server 基类完成
//    - 本类只需要重写 handleClient() 处理每个客户端连接即可
// ============================================================================
#ifndef SMTP_SERVER_H
#define SMTP_SERVER_H

#include "Server.h"   // 引入基类（Server 基类声明了纯虚函数 handleClient）
#include <string>
#include <vector>

// 用于存储一封邮件的临时结构
// SMTP 发信是"分步进行"的：MAIL FROM（发件人）→ RCPT TO（收件人）→ DATA（正文），
// 服务器必须把这几步的信息拼在一起，才能凑成完整的一封邮件，
// 所以用一个结构体作为"会话状态"，跨命令共享
struct SmtpMail {
    std::string from;   // 发件人地址（来自 MAIL FROM:<xxx>）
    std::string to;     // 收件人地址（来自 RCPT TO:<xxx>）
    std::string body;   // 邮件正文，包含头部和正文（DATA 阶段逐行积累）
};

class SmtpServer : public Server {
public:
    // 构造函数：port 是要监听的端口
    // 会把端口号传给 Server 基类，同时创建 ./mailbox 存储目录（见 .cpp）
    explicit SmtpServer(int port);
    virtual ~SmtpServer() = default;   // 析构函数用默认实现即可（没有额外资源要释放）

protected:
    // 重写 handleClient，实现 SMTP 协议逻辑
    // 这是 Server 基类声明的纯虚函数，必须实现。
    // 每当一个客户端连接进来，Server::start() 会开一个新线程调用它，
    // 线程结束时这个客户端的会话也就结束了
    void handleClient(int client_fd) override;

private:
    // 解析并处理单个命令，返回 true 表示继续会话，false 表示断开（QUIT）
    // fd       : 当前客户端的 socket 句柄（用于发响应给客户端）
    // line     : 客户端发来的一行内容（已去掉 \r\n）
    // mail     : 正在拼装的邮件，跨命令共享（是"会话状态"）
    // dataMode : 是否为 DATA 模式。用引用传递，因为函数内要修改它的值：
    //             普通模式下 line 是一条命令；DATA 模式下 line 是正文的一行
    bool processCommand(int fd, const std::string& line, SmtpMail& mail, bool& dataMode);

    // 发送响应行（给 response 补上 \r\n 后通过 socket 发出去）
    // SMTP 协议规定所有响应行必须以 \r\n 结尾，封装一层避免重复写
    void sendResponse(int fd, const std::string& response);

    // 保存邮件到文件（存储路径可配置，这里写死为 ./mailbox/）
    // 文件名用"时间戳_随机数.eml"，避免多线程并发时文件名冲突
    void saveMail(const SmtpMail& mail);
};

#endif // SMTP_SERVER_H
