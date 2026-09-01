#include "Server.h"
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

// 构造函数
// 注意：初始化顺序必须与头文件中的声明顺序一致（server_fd, port, is_running）
Server::Server(int port) 
    : server_fd(-1), port(port), is_running(false) {
}

// 析构函数：确保服务器停止并释放资源
Server::~Server() {
    stop();
}

// 启动服务器
bool Server::start() {
    // 1. 创建 TCP socket
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket 创建失败");
        return false;
    }

    // 2. 设置端口复用（SO_REUSEADDR），防止程序崩溃后端口被占用
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt 失败");
        close(server_fd);
        return false;
    }

    // 3. 绑定地址和端口
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));  // 清空结构体
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;  // 监听所有网卡
    address.sin_port = htons(port);        // 转为网络字节序

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind 失败（端口被占用？）");
        close(server_fd);
        return false;
    }

    // 4. 开始监听，最大等待队列为 5
    if (listen(server_fd, 5) < 0) {
        perror("listen 失败");
        close(server_fd);
        return false;
    }

    // 5. 服务器启动成功，进入主循环
    is_running = true;
    std::cout << "[服务器] 已启动，监听端口 " << port << std::endl;

    // 6. 主循环：不断接受新连接
    while (is_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        // accept 阻塞等待新连接
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            // 如果服务器被停止，accept 会失败，这里检查 is_running 来判断是否正常退出
            if (!is_running) {
                break;  // 正常退出循环
            }
            perror("accept 失败");
            continue;   // 非致命错误，继续等待下一个连接
        }

        // 打印客户端 IP 和端口（用于调试）
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        std::cout << "[服务器] 新连接来自 " << client_ip << ":" 
                  << ntohs(client_addr.sin_port) << std::endl;

        // ！！！多线程处理：为每个客户端创建一个新线程，并调用 handleClient
        // 注意：这里直接创建 std::thread，并 detach 让线程独立运行
        // 更高级的做法是使用线程池，这里为了简单易懂，我们采用每连接一线程
        std::thread worker([this, client_fd]() {
            // 调用子类实现的 handleClient 处理业务逻辑
            this->handleClient(client_fd);
            // 处理完成后，关闭客户端的 socket（子类也可以关闭，但这里作为保险）
            close(client_fd);
        });
        worker.detach();  // 分离线程，主线程不再等待它
    }

    // 7. 退出循环，关闭监听套接字
    close(server_fd);
    std::cout << "[服务器] 已停止" << std::endl;
    return true;
}

// 停止服务器
void Server::stop() {
    if (is_running) {
        is_running = false;
        // 为了快速退出 accept 阻塞，可以在这里关闭 server_fd
        // 这样 accept 会立即返回 -1，循环就会退出
        if (server_fd != -1) {
            close(server_fd);
            server_fd = -1;
        }
    }
}