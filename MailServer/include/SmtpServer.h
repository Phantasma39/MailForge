#ifndef SMTP_SERVER_H
#define SMTP_SERVER_H

#include "Server.h"   // 引入基类（Server 基类声明了纯虚函数 handleClient）
#include <string>
#include <vector>

// 用于存储一封邮件的临时结构
// SMTP 发信是"分步进行"的：MAIL FROM（发件人）→ RCPT TO（收件人）→ DATA（邮件内容），
// 服务器必须把这几步的信息拼在一起，才能凑成完整的一封邮件，
// 所以用一个结构体作为"会话状态"，跨命令共享
// 注意：按 RFC 5321 / RFC 5322 的规定，DATA 内容本身就是一封完整的邮件
// （头部区 + 空行 + 正文），所以下面的 body 字段存的是"完整邮件原文"，
// DATA 结束后由 parseMailData() 负责把其中的标准头解析出来
struct SmtpMail {
    std::string from;   // 信封发件人地址（来自 MAIL FROM:<xxx>）
    std::string to;     // 信封收件人地址（来自 RCPT TO:<xxx>）
    std::string body;   // DATA 阶段逐行收到的邮件原文（头部 + 空行 + 正文）

    // ---- 下面这些字段在 DATA 结束时由 parseMailData() 解析填充 ----
    std::string headers;    // 客户端在 DATA 里发的头部区原文（第一个空行之前，原样保留）
    std::string text;       // 正文（第一个空行之后的部分）
    std::string headerFrom; // 头部区中 From: 的值（客户端没写则为空）
    std::string headerTo;   // 头部区中 To: 的值
    std::string subject;    // 头部区中 Subject: 的值（客户端没写则为空）
    std::string headerDate; // 头部区中 Date: 的值
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

    bool sendAll(int fd, const char* data, size_t len);

    bool processCommand(int fd, const std::string& line, SmtpMail& mail, bool& dataMode);

    // 发送响应行（给 response 补上 \r\n 后通过 socket 发出去）
    // SMTP 协议规定所有响应行必须以 \r\n 结尾，封装一层避免重复写
    void sendResponse(int fd, const std::string& response);

    // 解析 DATA 阶段收齐的邮件原文（mail.body）
    // 按 RFC 5322 邮件格式把原文拆成"头部(headers) + 正文(text)"，
    // 并提取 From / To / Subject / Date 等标准头字段，
    // 这样保存邮件时客户端真正写的这些头就不会被忽略
    void parseMailData(SmtpMail& mail);

    // 在头部原文中查找某个头字段（如 "From"、"Subject"）的值
    // 字段名大小写不敏感（RFC 5322：字段名不区分大小写），找不到返回空串
    std::string getHeaderValue(const std::string& headers, const std::string& name);

    // 保存邮件到文件（存储路径可配置，这里写死为 ./mailbox/）
    // 文件名用"时间戳_随机数.eml"，避免多线程并发时文件名冲突
    void saveMail(const SmtpMail& mail);
};

#endif // SMTP_SERVER_H
