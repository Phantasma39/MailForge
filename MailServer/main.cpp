// ============================================================================
//  main.cpp —— 程序的主入口
//
//  整个 MailServer 程序从这里启动：
//    1. 创建一个 SmtpServer 对象（SMTP 服务器，监听 2525 端口）
//    2. 调用 server.start() 进入阻塞式运行，直到进程被杀死
//
//  设计要点：
//    - 使用 2525 端口而不是标准的 25 端口：
//      在 Linux 上 25 端口需要 root 权限，而且容易被防火墙拦截；
//      2525 是高端口，普通用户就能直接监听，方便本地开发测试。
//    - start() 内部是一个死循环（accept 循环），所以程序会一直"卡"在
//      server.start() 这一行运行，正常情况下不会走到 return 0
// ============================================================================
#include "SmtpServer.h"   // SmtpServer 类的声明（它继承自 Server 基类）
#include <iostream>

// main 函数：C++ 程序的统一入口
int main() {
    // 创建 SMTP 服务器对象，指定监听端口为 2525
    // SmtpServer 的构造函数会：
    //   1. 调用 Server 基类构造函数，把端口号保存起来
    //   2. 创建 ./mailbox 邮件存储目录（见 SmtpServer.cpp）
    SmtpServer server(2525);

    // 打印一行启动提示（方便用户看到程序跑起来了）
    std::cout << "启动 SMTP 服务器，端口 2525" << std::endl;

    // 启动服务器（阻塞运行）
    // server.start() 内部会依次完成：
    //   socket 创建 → setsockopt 端口复用 → bind 绑定 → listen 监听
    //   → while 死循环 accept 新连接（每来一个客户端开一个线程处理）
    // 所以这一行会一直运行不返回，直到进程被 Ctrl+C 杀死
    server.start();

    // 正常情况永远执行不到这里（start() 是死循环）
    return 0;
}
