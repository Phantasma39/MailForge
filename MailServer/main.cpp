//程序的主入口

#include "SmtpServer.h"
#include <iostream>

int main() {
    // 监听 2525 端口（避开 root 权限）
    SmtpServer server(2525);
    std::cout << "启动 SMTP 服务器，端口 2525" << std::endl;
    server.start(); // 阻塞运行，直到按 Ctrl+C 或调用 stop()
    return 0;
}