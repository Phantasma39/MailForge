// ============================================================================
//  main.cpp —— 程序的主入口
//
//  整个 MailServer 程序从这里启动：
//    1. 创建一个 SmtpServer 对象（SMTP 服务器，监听 2525 端口）
//    2. 创建一个 Pop3Server 对象（POP3 服务器，监听 1110 端口）
//    3. 分别在两个线程里调用 start() 进入阻塞式运行，直到进程被杀死
//
//  设计要点：
//    - SMTP 使用 2525 端口而不是标准的 25 端口：
//      在 Linux 上 25 端口需要 root 权限，而且容易被防火墙拦截；
//      2525 是高端口，普通用户就能直接监听，方便本地开发测试。
//    - 同理 POP3 使用 1110 端口（标准 POP3 是 110，也需要 root 权限）。
//    - 两个服务器各占一个线程跑自己的 accept 死循环；
//      start() 会一直"卡"在各自的循环里，所以正常情况下程序不会走到 return 0。
// ============================================================================
#include "SmtpServer.h"   // SmtpServer 类的声明（它继承自 Server 基类）
#include "Pop3Server.h"   // Pop3Server 类的声明（同样继承自 Server 基类）
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

    std::cout << "==============================================" << std::endl;
    std::cout << " MailForge MailServer 启动" << std::endl;
    std::cout << "   SMTP 服务器：端口 2525（发邮件用）" << std::endl;
    std::cout << "   POP3 服务器：端口 1110（收邮件用）" << std::endl;
    std::cout << "==============================================" << std::endl;

    // 两个服务器都要"同时"跑，而 start() 是死循环（accept 循环），
    // 所以让 POP3 在一条线程里跑，主线程跑 SMTP；
    // （两个线程里各自执行：socket 创建 → setsockopt 端口复用 → bind → listen
    //   → while 死循环 accept 新连接，每来一个客户端开一个线程处理）
    std::thread pop3Thread([&pop3Server]() {
        pop3Server.start();
    });

    // 主线程阻塞在 SMTP 服务器的 accept 循环里，直到进程被 Ctrl+C 杀死
    smtpServer.start();

    // 正常情况下永远执行不到这里（start() 是死循环）
    pop3Thread.join();
    return 0;
}
