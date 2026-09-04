// ============================================================================
//  SmtpClient.h —— SMTP 客户端类
//
//  作用：
//    和服务端的 SmtpServer 正好"角色对调"：
//      服务端 SmtpServer 是"等别人连上来发信给我"；
//      客户端 SmtpClient 是"我主动连上服务器的 2525 端口，把信发出去"。
//
//    一个完整的 SMTP 发送会话（本类 sendMail 内部自动完成）：
//      连接 ─► 220 服务器问候
//           ─► 客户端: EHLO 主机名      （打招呼）
//           ─► 服务器: 250 ...
//           ─► 客户端: MAIL FROM:<发件人>
//           ─► 服务器: 250 OK
//           ─► 客户端: RCPT TO:<收件人>
//           ─► 服务器: 250 OK
//           ─► 客户端: DATA
//           ─► 服务器: 354 开始发内容
//           ─► 客户端: 头部 + 空行 + 正文（正文里 . 开头的行要写成 ..）
//           ─► 客户端: .（单独一个点，表示内容结束）
//           ─► 服务器: 250 邮件已接收
//           ─► 客户端: QUIT
//           ─► 服务器: 221 再见
//
//  用法（三步）：
//    SmtpClient smtp;                              // 默认连 127.0.0.1:2525
//    bool ok = smtp.sendMail("a@example.com",
//                            "b@example.com",
//                            "标题", "正文");
//    if (!ok) std::cout << smtp.getLastError();
// ============================================================================
#ifndef SMTP_CLIENT_H
#define SMTP_CLIENT_H

#include <string>

class SmtpClient {
public:
    // 构造函数1：不传参 = 连本机 127.0.0.1:2525（本项目 MailServer 的 SMTP 端口）
    SmtpClient();

    // 构造函数2：指定服务器地址与端口（以后连公网邮箱也能用）
    SmtpClient(const std::string& server, int port);

    // 析构：自动关闭 socket
    ~SmtpClient();

    // ★ 核心方法：发一封邮件
    // 输入：
    //   from    发件人地址，如 "alice@example.com"
    //   to      收件人地址，如 "bob@example.com"
    //   subject 邮件主题（支持 UTF-8 中文，直接传字符串即可）
    //   body    正文（支持 UTF-8 中文；内部自动按行拆分并做 SMTP 点填充）
    // 返回值：
    //   true  = 邮件已被服务器接受（服务器回复了 250）
    //   false = 中间任何一步失败（可用 getLastError() 查看原因）
    bool sendMail(const std::string& from,
                  const std::string& to,
                  const std::string& subject,
                  const std::string& body);

    // ★ 发一封"完整原始邮件"：头部由调用方自己拼好，原文整段发出
    // 用途：加密场景下，先对整封邮件加密（见 MailCrypto::encryptPayload），
    //       再把密文作为 rawMail 发出去；服务器侧原样存盘。
    // 返回值：true = 服务器已接受。
    bool sendRawMail(const std::string& from,
                     const std::string& to,
                     const std::string& rawMail);

    // 主动关闭连接（析构函数也会自动调用，一般不用手动调）
    void close();

    // 最近一次错误信息（发送失败时用来排查）
    std::string getLastError() const { return lastError_; }

private:
    int sockFd_;             // 与服务端通信的 socket（-1 表示未连接）
    std::string server_;     // 服务器地址，如 127.0.0.1
    int port_;               // 服务器端口，如 2525
    std::string lastError_;  // 最近一次错误的描述

    // 建立 TCP 连接（并设置 5 秒收发超时，防止一直卡死）
    bool connectServer();

    // 发送一行 SMTP 命令（自动补 \r\n）
    bool sendLine(const std::string& line);

    // 从服务器读一行响应（去掉行尾 \r\n），存进 line
    bool recvLine(std::string& line);

    // 等待服务器回一个"指定状态码"的响应（如 expectCode=250）
    // 自动跳过 250-xxx 形式的多行响应
    bool waitReply(int expectCode);

    // 把一整段文本按行发出去（每行做 SMTP 点填充），最后发单独的 "." 结束
    bool sendRawContent(const std::string& rawMail);
};

#endif // SMTP_CLIENT_H
