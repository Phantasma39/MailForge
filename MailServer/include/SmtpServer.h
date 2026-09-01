#ifndef SMTP_SERVER_H
#define SMTP_SERVER_H

#include "Server.h"
#include <string>
#include <vector>

// 用于存储一封邮件的临时结构
struct SmtpMail {
    std::string from;
    std::string to;
    std::string body;      // 包括头部和正文
};

class SmtpServer : public Server {
public:
    explicit SmtpServer(int port);
    virtual ~SmtpServer() = default;

protected:
    // 重写 handleClient，实现 SMTP 协议逻辑
    void handleClient(int client_fd) override;

private:
    // 解析并处理单个命令，返回 true 表示继续会话，false 表示断开
    bool processCommand(int fd, const std::string& line, SmtpMail& mail, bool& dataMode);
    
    // 发送响应行
    void sendResponse(int fd, const std::string& response);
    
    // 保存邮件到文件（存储路径可配置，这里写死为 ./mailbox/）
    void saveMail(const SmtpMail& mail);
};

#endif // SMTP_SERVER_H