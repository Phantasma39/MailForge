// ============================================================================
//  HttpServer.h —— HTTP 服务器类（Web 后端）
//
//  作用：
//    给浏览器提供 HTTP 接口（端口 8080）。浏览器不能直接说 SMTP/POP3，
//    于是由这个服务器当"翻译官"：
//      浏览器 ──HTTP──► HttpServer(8080) ──SMTP/POP3客户端──► MailServer(2525/1110)
//
//  对外接口一览（都用 GET/POST + 表单参数，返回 JSON）：
//    POST /api/login    参数: user, pass          → 登录，返回 {token}
//    POST /api/logout   参数: token               → 退出登录
//    POST /api/send     参数: token,to,subject,body[,from][,encrypt]
//                                              → 发邮件（encrypt=1 走加密通道）
//    GET  /api/inbox?token=xxx                    → 收件箱列表（含主题/发件人）
//    GET  /api/mail?token=xxx&n=编号              → 读某一封完整原文
//    POST /api/delete   参数: token, n            → 删除某一封（POP3 DELE+QUIT）
//    GET  /             → 静态页面（web/ 目录）
//
//  登录校验设计：直接拿用户名密码去连本机 POP3(1110) 试登录，
//  能过就说明账号有效——这样 Web 登录和 POP3 认证天然是同一套账号（users.txt）。
//
//  加密挂钩点：
//    /api/send 与收信解析处已经预留了 MailCrypto 调用（见 MailCrypto.h），
//    encrypt=1 时发信前对整封邮件原文加密（当前 XOR 可运行，AES/RC4 留 TODO）。
// ============================================================================
#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include "Server.h"
#include <string>
#include <map>
#include <vector>
#include <mutex>

// 一次 HTTP 请求的解析结果
struct HttpRequest {
    std::string method;                    // GET / POST
    std::string path;                      // 请求路径（不含 ?query）
    std::string version;                   // HTTP/1.1
    std::map<std::string, std::string> query;   // URL 问号后面的参数（GET）
    std::map<std::string, std::string> form;    // POST 表单体里的参数（application/x-www-form-urlencoded）
};

// HTTP 响应
struct HttpResponse {
    int status = 200;                      // 状态码
    std::string statusText = "OK";         // 状态文本
    std::string contentType = "text/plain";
    std::string body;
};

class HttpServer : public Server {
public:
    explicit HttpServer(int port);

    // 从一封邮件的原文里取某个头部字段（如 Subject），找不到返回空串。
    // 声明成 public：文件内 decodeMail()（加密邮件还原）等工具函数也要用它
    static std::string parseHeader(const std::string& rawMail, const std::string& name);

protected:
    // 重写 Server 的纯虚函数：处理一个浏览器连接（读请求 → 路由 → 回响应）
    void handleClient(int client_fd) override;

private:
    // 登录会话：token → 用户信息
    struct Session {
        std::string user;   // 登录名（如 bob）
        std::string pass;   // 密码（本课程演示用明文保存；生产环境应存哈希）
    };
    std::map<std::string, Session> sessions_;   // 已登录会话表
    std::mutex sessionsMutex_;                  // 保护会话表（多线程同时登录/退出）

    // ---------- HTTP 底层 ----------
    bool readLine(int fd, std::string& line);                    // 读一行（去 \r\n）
    bool readRequest(int fd, HttpRequest& req);                  // 解析完整请求
    static void parseKeyValues(const std::string& raw,
                               std::map<std::string, std::string>& out);   // 解析 a=b&c=d
    static std::string urlDecode(const std::string& s);          // URL 解码
    void sendHttp(int fd, const HttpResponse& resp);             // 把响应发回去

    // ---------- 路由 ----------
    void route(const HttpRequest& req, HttpResponse& resp);      // 分发到 GET/POST
    void handleStatic(const HttpRequest& req, HttpResponse& resp);   // 网页文件
    void handleApi(const HttpRequest& req, HttpResponse& resp);      // /api/xxx

    // 各 /api 接口的具体实现（handler）
    void handleLogin(const HttpRequest& req, HttpResponse& resp);    // POST /api/login
    void handleLogout(const HttpRequest& req, HttpResponse& resp);   // POST /api/logout
    void handleSend(const HttpRequest& req, HttpResponse& resp);     // POST /api/send
    void handleInbox(const HttpRequest& req, HttpResponse& resp);    // GET /api/inbox
    void handleMail(const HttpRequest& req, HttpResponse& resp);     // GET /api/mail
    void handleDelete(const HttpRequest& req, HttpResponse& resp);   // POST /api/delete

    // ---------- 业务辅助 ----------
    bool loginAndGetSession(const std::string& token,
                            Session& out);                 // 由 token 找会话
    void makeSession(const std::string& user, const std::string& pass,
                     std::string& tokenOut);                     // 生成 token 并存会话
    std::string randomToken() const;                             // 生成随机 token

    // 生成一个 JSON 字符串（转义了引号/换行等）
    static std::string jsonEscape(const std::string& s);

    // 返回形如 {"ok":true/false,"msg":"..."} 的 JSON
    static std::string jsonResult(bool ok, const std::string& msg);
};

#endif // HTTP_SERVER_H
