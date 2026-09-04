// ============================================================================
//  main.cpp —— 程序的主入口
//
//  整个 MailServer 程序从这里启动：
//    1. 创建一个 SmtpServer 对象（SMTP 服务器，监听 2525 端口）
//    2. 创建一个 Pop3Server 对象（POP3 服务器，监听 1110 端口）
//    3. 创建一个 HttpServer 对象（HTTP 服务器，监听 8080 端口，Web 后端）
//    4. 分别在不同线程里调用 start() 进入阻塞式运行，直到进程被杀死
//
//  设计要点：
//    - SMTP 使用 2525 端口而不是标准的 25 端口：
//      在 Linux 上 25 端口需要 root 权限，而且容易被防火墙拦截；
//      2525 是高端口，普通用户就能直接监听，方便本地开发测试。
//    - 同理 POP3 使用 1110 端口（标准 POP3 是 110，也需要 root 权限）。
//    - HTTP 用 8080 端口：浏览器唯一能直接访问的入口。
//      浏览器 → HTTP(8080) → 后端内部再走 SMTP(2525) 发信 / POP3(1110) 收信。
//    - 三个服务器各占一个线程跑自己的 accept 死循环；
//      start() 会一直"卡"在各自的循环里，所以正常情况下程序不会走到 return 0。
// ============================================================================
#include "SmtpServer.h"   // SmtpServer 类的声明（它继承自 Server 基类）
#include "Pop3Server.h"   // Pop3Server 类的声明（同样继承自 Server 基类）
#include "HttpServer.h"   // HttpServer 类的声明（Web 后端，同样继承自 Server）
#include <iostream>
#include <thread>

// main 函数：C++ 程序的统一入口
int main() {
    // 创建 SMTP 服务器对象，指定监听端口为 2525
    // SmtpServer 的构造函数会：
    //   1. 调用 Server 基类构造函数，把端口号保存起来
    //   2. 创建 ./mailbox 邮件存储目录（见 SmtpServer.cpp）
    SmtpServer smtpServer(2525);

    // 创建 POP3 服务器对象，指定监听端口为 1110
    // Pop3Server 的构造函数会：
    //   1. 调用 Server 基类构造函数，把端口号保存起来
    //   2. 确保 ./mailbox 存在，并加载 ./users.txt 里的账号表（见 Pop3Server.cpp）
    Pop3Server pop3Server(1110);

    // 创建 HTTP 服务器对象，指定监听端口为 8080（Web 后端，浏览器访问入口）
    // HttpServer 的构造函数会：调用 Server 基类构造函数，并确保 ./web 目录存在
    HttpServer httpServer(8080);

    std::cout << "===============================================" << std::endl;
    std::cout << " MailForge MailServer 启动" << std::endl;
    std::cout << "   SMTP 服务器：端口 2525（邮件协议发信用）" << std::endl;
    std::cout << "   POP3 服务器：端口 1110（邮件协议收信用）" << std::endl;
    std::cout << "   HTTP 服务器：端口 8080（浏览器/Web 后端用）" << std::endl;
    std::cout << "===============================================" << std::endl;

    // 三个服务器都要"同时"跑，而 start() 是死循环（accept 循环），
    // 所以 POP3 和 HTTP 各自占一条线程，主线程跑 SMTP。
    std::thread pop3Thread([&pop3Server]() {
        pop3Server.start();
    });
    std::thread httpThread([&httpServer]() {
        httpServer.start();
    });

    // 主线程阻塞在 SMTP 服务器的 accept 循环里，直到进程被 Ctrl+C 杀死
    smtpServer.start();

    // 正常情况下永远执行不到这里（start() 是死循环）
    httpThread.join();
    pop3Thread.join();
    return 0;
}
