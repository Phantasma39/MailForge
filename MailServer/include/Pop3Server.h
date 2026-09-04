// ============================================================================
//  Pop3Server.h —— POP3 服务器类（继承自 Server 基类）
//
//  作用：
//    实现 POP3（Post Office Protocol version 3，邮局协议第三版）的服务器端。
//    POP3 负责"收邮件"：客户端（Outlook / Foxmail / python poplib 等）登录后，
//    把服务器上属于自己的邮件下载到本地，协议规范见 RFC 1939。
//
//    和 SMTP 一样，POP3 也是基于 TCP 的文本协议，一问一答：
//      客户端发一行命令（USER、PASS、STAT、LIST、RETR、DELE、QUIT...）
//      服务器回一行状态：
//        +OK  表示成功
//        -ERR 表示失败
//        多行内容（邮件正文、邮件列表）以单独一行的 "." 表示结束
//
//  一个 POP3 会话有三个阶段（这就是 POP3 的状态机）：
//    1. AUTHORIZATION（认证态）：还没登录，只允许 USER / PASS / QUIT
//    2. TRANSACTION（事务态）： 登录成功后，可以 STAT / LIST / RETR / DELE 等
//    3. UPDATE（更新态）：      QUIT 退出时，把被 DELE 标记的邮件真正从磁盘删掉
//
//  邮件存储约定（和 SmtpServer::saveMail 配套使用）：
//    每个用户一个收件目录 ./mailbox/<用户名>/，用户名取"收件人邮箱 @ 前面那部分"并转小写。
//    例如 SMTP 收到一封发给 bob@example.com 的邮件 → 投递到 ./mailbox/bob/xxx.eml，
//    那么 bob 用 POP3 登录后，读到的就是 ./mailbox/bob/ 下的这些 .eml，
//    这样就实现了 README 里说的"多邮箱账户隔离"。
//
//  账号密码：
//    保存在 ./users.txt 里，一行一个「用户名:密码」（用户名也会按上面的规则规范化），
//    文件不存在时会用内置默认账号兜底（见 loadAccounts）。
// ============================================================================
#ifndef POP3_SERVER_H
#define POP3_SERVER_H

#include "Server.h"   // 基类（和 SmtpServer 一样，网络部分全部复用 Server）
#include <string>
#include <vector>
#include <map>

// 表示用户邮箱里的"一封邮件"
// 登录成功后，服务器会把该用户目录下的 .eml 全部读进内存做成快照，
// 之后 STAT / LIST / RETR / DELE 都是对着这份快照操作
struct Pop3Mail {
    std::string path;     // 完整文件路径，如 ./mailbox/bob/1788314490_846930886.eml
    long long size;       // 文件大小（字节数），STAT / LIST 的 octets 就是它
    bool deleted;         // 本会话中是否已被 DELE 标记删除（只是标记！QUIT 时才真删文件）
};

// 一次 POP3 会话的状态（跨命令共享，作用类似 SmtpServer.h 里的 SmtpMail）
struct Pop3State {
    std::string username;                 // USER 命令传来的登录名原文（如 bob@example.com）
    std::string userKey;                  // 规范化后的用户名（小写、只留 @ 前部分），用来找目录 / 查账号
    bool authed = false;                  // PASS 是否验证通过（true 后进入 TRANSACTION 态）
    std::vector<Pop3Mail> mails;          // 当前用户的邮箱快照（会话期间编号 1..N 固定不变）
};

class Pop3Server : public Server {
public:
    // 构造函数：port 是要监听的端口
    // 会把端口号传给 Server 基类，同时确保 ./mailbox 存在并加载 ./users.txt 账号表
    explicit Pop3Server(int port);
    virtual ~Pop3Server() = default;

protected:
    // 重写 Server 的纯虚函数 handleClient，实现 POP3 协议逻辑（和 SmtpServer 一样）
    void handleClient(int client_fd) override;

private:
    std::map<std::string, std::string> accounts_;   // 账号表：规范化用户名 → 密码

    // 保证一次把数据发完（send 不一定一次发完，可能被中断，要循环发），和 SMTP 完全一样
    bool sendAll(int fd, const char* data, size_t len);

    // 发一行响应（自动补 \r\n），POP3 所有响应行都必须以 \r\n 结尾
    void sendResponse(int fd, const std::string& response);

    // 加载 ./users.txt 里的账号表；文件不存在 / 内容为空时用内置默认账号兜底
    void loadAccounts();

    // 把用户输入的登录名规范成"小写 + 只留 @ 前部分"
    // （bob@example.com、Bob、bob 都会变成 bob，方便和目录名、账号表对上）
    std::string normalizeUser(const std::string& input) const;

    // 返回某用户的收件目录路径，如 bob → "./mailbox/bob"
    std::string mailboxDir(const std::string& userKey) const;

    // 把某用户收件目录下所有 .eml 读进 mails（按文件名排序，保证每次会话顺序稳定）
    // 目录不存在就当作空邮箱（0 封邮件），不报错
    void loadUserMails(const std::string& userKey, std::vector<Pop3Mail>& mails);

    // 解析并处理单条命令；返回 true 表示继续会话，false 表示断开（QUIT）
    // st 是本会话的共享状态（登录名 / 是否通过认证 / 邮箱快照）
    bool processCommand(int fd, const std::string& line, Pop3State& st);

    // 把一行拆成"命令 + 参数"（和 SmtpServer 里的拆法一样），命令转大写
    static void splitCommand(const std::string& line, std::string& cmd, std::string& args);

    // 把参数解析成消息编号（纯数字）；非法返回 0（POP3 消息编号从 1 开始，0 代表"没解析出来"）
    static int parseMsgNum(const std::string& args);

    // 判断编号 num 在当前邮箱快照里是否合法（在 1..N 范围内且未被 DELE 标记）
    bool isValidMsg(int num, const Pop3State& st) const;

    // 统计当前邮箱"还没被标记删除"的邮件数量与总字节数（STAT 和 LIST 的总览行都用它）
    void statMailbox(const Pop3State& st, int& count, long long& totalBytes) const;

    // RETR 时把邮件文件按 POP3 的"点填充"规则发出去：
    // 内容里以 "." 开头的行要写成 ".."，最后再发单独一行的 "." 表示正文结束
    void sendMailContent(int fd, const std::string& path);
};

#endif // POP3_SERVER_H
