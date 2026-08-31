//你要知道，这是最强的主文件，所有的入口
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "utils.h"

// 处理单个客户端的连接（里程碑1：只接收数据并回显）
void handle_client(int client_fd) {
    char buffer[BUFFER_SIZE] = {0};
    
    // 1. 先发一条欢迎语给客户端（模仿 SMTP 的 220 响应）
    std::string welcome = "220 MyMailServer Ready\r\n";
    send(client_fd, welcome.c_str(), welcome.length(), 0);
    std::cout << "[服务器] 发送欢迎语" << std::endl;

    // 2. 循环接收客户端发来的数据
    while (true) {
        memset(buffer, 0, BUFFER_SIZE);
        int bytes_read = recv(client_fd, buffer, BUFFER_SIZE - 1, 0);
        
        if (bytes_read <= 0) {
            std::cout << "[服务器] 客户端断开连接" << std::endl;
            break;
        }

        // 打印客户端发来的内容（你后续要解析的就是这个！）
        std::cout << "[服务器] 收到: " << buffer;

        // ★ 里程碑1：无论客户端发什么，我们都回复 "250 OK" ★
        // 等你进入里程碑2，这里会改成 if (EHLO) / if (MAIL FROM)...
        std::string reply = "250 OK - 服务器已收到\r\n";
        send(client_fd, reply.c_str(), reply.length(), 0);
    }

    close(client_fd);
}

int main() {
    int server_fd, client_fd;
    struct sockaddr_in address;
    int addrlen = sizeof(address);
    int opt = 1;

    // 1. 创建 TCP Socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket 创建失败");
        return -1;
    }

    // 2. 设置端口复用（防止程序崩溃后端口被占用）
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt 失败");
        return -1;
    }

    // 3. 绑定地址和端口
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;  // 监听所有网卡（0.0.0.0）
    address.sin_port = htons(SERVER_PORT); // 端口 8888

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind 失败（端口被占用？）");
        return -1;
    }

    // 4. 开始监听
    if (listen(server_fd, 5) < 0) { // 5 表示最大等待队列长度
        perror("listen 失败");
        return -1;
    }

    std::cout << "========================================" << std::endl;
    std::cout << "[服务器] 已启动！" << std::endl;
    std::cout << "[服务器] 监听端口: " << SERVER_PORT << std::endl;
    std::cout << "[服务器] 等待客户端连接..." << std::endl;
    std::cout << "========================================" << std::endl;

    // 5. 主循环：不断接受新客户端
    while (true) {
        if ((client_fd = accept(server_fd, (struct sockaddr*)&address, (socklen_t*)&addrlen)) < 0) {
            perror("accept 失败");
            continue;
        }

        // 打印客户端的 IP 地址（方便你确认是谁连上来了）
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &address.sin_addr, client_ip, INET_ADDRSTRLEN);
        std::cout << "[服务器] 新客户端连接: " << client_ip << ":" << ntohs(address.sin_port) << std::endl;

        // ★ 里程碑1：直接在当前线程处理（不创建新线程，简单优先）★
        // 等跑通了，下一阶段我们再改成多线程
        handle_client(client_fd);
    }

    close(server_fd);
    return 0;
}