#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <ctime>

#define PORT 8888
#define BUFFER_SIZE 4096

void handle_client(int client_fd) {
    char buffer[BUFFER_SIZE] = {0};
    
    // 接收客户端的请求（我们暂时不解析，直接打印出来看看）
    int bytes_read = recv(client_fd, buffer, BUFFER_SIZE - 1, 0);
    if (bytes_read > 0) {
        std::cout << "[服务器] 收到请求:\n" << buffer << std::endl;
    }

    // 构造 HTTP 响应（返回一个简单的 HTML 页面）
    std::string html_content = 
        "<!DOCTYPE html>"
        "<html>"
        "<head><meta charset='UTF-8'><title>我的邮件系统</title></head>"
        "<body style='font-family: Arial; text-align: center; margin-top: 50px;'>"
        "<h1 style='color: #2c3e50;'>✅ 你能收到我的消息了，你很强！</h1>"
        "<p>当前时间: " + std::to_string(time(nullptr)) + "</p>"
        "<p>你通过浏览器访问到了 C++ 后端！</p>"
        "</body>"
        "</html>";

    std::string http_response = 
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: " + std::to_string(html_content.size()) + "\r\n"
        "Connection: close\r\n"
        "\r\n" +
        html_content;

    send(client_fd, http_response.c_str(), http_response.length(), 0);
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
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind 失败（端口被占用？）");
        return -1;
    }

    // 4. 开始监听
    if (listen(server_fd, 5) < 0) {
        perror("listen 失败");
        return -1;
    }

    std::cout << "========================================" << std::endl;
    std::cout << "[服务器] 已启动！" << std::endl;
    std::cout << "[服务器] 监听端口: " << PORT << std::endl;
    std::cout << "[服务器] 等待连接..." << std::endl;
    std::cout << "========================================" << std::endl;

    // 5. 主循环：不断接受新客户端
    while (true) {
        if ((client_fd = accept(server_fd, (struct sockaddr*)&address, (socklen_t*)&addrlen)) < 0) {
            perror("accept 失败");
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &address.sin_addr, client_ip, INET_ADDRSTRLEN);
        std::cout << "[服务器] 新连接来自: " << client_ip << ":" << ntohs(address.sin_port) << std::endl;

        handle_client(client_fd);
    }

    close(server_fd);
    return 0;
}