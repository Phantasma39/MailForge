// ============================================================================
//  Pop3Client.h —— POP3 客户端类
//
//  作用：
//    和服务端的 Pop3Server 角色对调：主动连上服务器的 1110 端口，
//    登录后把属于"我"的邮件从服务器拉下来（列列表 / 收信 / 删信）。
//
//    一个典型的 POP3 收信会话：
//      连接 ─► +OK 服务器问候
//           ─► USER bob / PASS 123456     （登录）
//           ─► +OK mailbox ready, N message(s), ...
//           ─► STAT / LIST                （看看有几封、每封多大）
//           ─► RETR 1                     （下载第 1 封，多行内容以 . 结束）
//           ─► DELE 1                     （标记删除，只有 QUIT 才真删）
//           ─► QUIT                       （退出，此时被 DELE 的邮件才真正删除）
//
//  用法：
//    Pop3Client pop3;                            // 默认连 127.0.0.1:1110
//    if (!pop3.login("bob", "123456")) ...        // 登录（失败可用 getLastError 查原因）
//    int n; long long bytes;
//    pop3.stat(n, bytes);                         // 看有几封
//    std::vector<Pop3MailInfo> list;
//    pop3.list(list);                             // 拿到 编号+大小 列表
//    std::string mail;
//    pop3.retr(1, mail);                          // 下载第 1 封的完整原文
//    pop3.dele(1);                                // 标记删除
//    pop3.quit();                                 // 真正执行删除并退出
// ============================================================================
#ifndef POP3_CLIENT_H
#define POP3_CLIENT_H

#include <string>
#include <vector>

// 收件箱列表里"一封邮件"的信息（LIST 命令的返回结果）
struct Pop3MailInfo {
    int number;        // 消息编号（从 1 开始，本会话内固定不变）
    long long size;    // 邮件字节数
};

class Pop3Client {
public:
    // 构造函数1：默认连本机 127.0.0.1:1110（本项目 MailServer 的 POP3 端口）
    Pop3Client();

    // 构造函数2：指定服务器地址与端口
    Pop3Client(const std::string& server, int port);

    // 析构：自动断开连接（注意：若没发 QUIT，DELE 的标记不会生效，符合协议）
    ~Pop3Client();

    // 登录：建立连接（如尚未连接）并执行 USER + PASS
    // 输入：user 用户名（如 "bob"，也支持 "bob@example.com"）；pass 密码
    // 返回：true 登录成功（已进入事务态，可以收发命令）
    bool login(const std::string& user, const std::string& pass);

    // STAT：查询邮箱统计
    // 输出（引用参数，函数内写入）：count 未删除邮件数；totalBytes 总字节数
    bool stat(int& count, long long& totalBytes);

    // LIST：拉取未删除邮件列表（编号 + 大小）
    // 输出：mails 列表（函数会先清空再填入）
    bool list(std::vector<Pop3MailInfo>& mails);

    // RETR：下载第 number 封邮件的完整原文
    // 输入：number 消息编号；输出：rawMail 邮件原文（已自动做点填充还原）
    bool retr(int number, std::string& rawMail);

    // DELE：把第 number 封标记为删除（只是标记！QUIT 时服务器才真删）
    bool dele(int number);

    // RSET：撤销本次会话所有 DELE 标记（删错了反悔）
    bool rset();

    // NOOP：保活
    bool noop();

    // QUIT：正常退出。之前 DELE 标记的邮件会在服务器上被真正删除
    bool quit();

    // 主动断开（内部会调 quit 之前先发 QUIT？不会——由调用方决定；此方法只关 socket）
    void close();

    // 最近一次错误信息
    std::string getLastError() const { return lastError_; }

private:
    int sockFd_;             // 与服务端通信的 socket（-1 表示未连接）
    std::string server_;     // 服务器地址，如 127.0.0.1
    int port_;               // 服务器端口，如 1110
    std::string lastError_;  // 最近一次错误的描述

    // 建立 TCP 连接（并设置 5 秒收发超时）
    bool connectServer();

    // 发送一行命令（自动补 \r\n）
    bool sendLine(const std::string& line);

    // 读一行响应（去掉行尾 \r\n），存进 line
    bool recvLine(std::string& line);

    // 读一行并判断是不是 +OK 开头；是则返回 true
    // （顺便把这行原文存进 replyLine，方便调用方解析数字）
    bool readReply(std::string& replyLine);

    // 读"多行响应"直到单独一行的 "."，返回中间所有行
    // 同时自动做 POP3 点填充还原（.. 开头 → . 开头）
    // 输入：maxLines 可选上限（防御异常服务器）
    bool readMultiLines(std::vector<std::string>& lines);

    // 从 "+OK 2 516" 这种响应文本里解析两个数字
    static bool parseTwoNumbers(const std::string& reply, long long& a, long long& b);
};

#endif // POP3_CLIENT_H
