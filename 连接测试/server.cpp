// ============================================================================
//  server.cpp —— 连通性测试服务器（里程碑 1：HTTP 连通性服务器）
//
//  作用：
//    这是一个"最简单能跑的" TCP + HTTP 服务器：
//    浏览器访问 → 服务器收到 HTTP 请求 → 返回一个写死的 HTML 页面。
//    用来验证：socket 编程通了、局域网/ZeroTier 能访问到本机。
//
//  注意：
//    - 这个版本是"单线程"的：一个客户端没处理完，其他客户端要排队等
//      （正式的 MailServer 用了多线程，见 MailServer/src/Server.cpp）
//    - 不解析 HTTP 请求内容，只是把收到的原始字节打印出来看
// ============================================================================
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <ctime>

// 监听端口（用高端口，避开系统保留端口，普通用户即可绑定）
#define PORT 3225
// 接收缓冲区大小（recv 一次最多读多少字节）
#define BUFFER_SIZE 4096

// ============================================================================
//  处理一个客户端连接
//  流程：收请求 → 打印 → 拼一个 HTTP 响应 → 发回去 → 关连接
//  参数 client_fd：accept 返回的客户端 socket，负责跟这个客户端收发数据
// ============================================================================
void handle_client(int client_fd) {
    char buffer[BUFFER_SIZE] = {0};   // 接收缓冲区，{0} 表示全部清零

    // 接收客户端的请求（我们暂时不解析，直接打印出来看看）
    // recv 返回实际读到的字节数；>0 表示收到了数据
    int bytes_read = recv(client_fd, buffer, BUFFER_SIZE - 1, 0);
    if (bytes_read > 0) {
        std::cout << "[服务器] 收到请求:\n" << buffer << std::endl;
    }

    // 构造 HTTP 响应（返回一个简单的 HTML 页面）
    // std::string 可以用 + 拼接；time(nullptr) 返回当前 Unix 时间戳
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

    // 拼装 HTTP 响应报文：状态行 + 若干响应头 + 空行 + 响应体
    //  \r\n 是 HTTP 协议规定的行结束符，一个都不能少
    //  Content-Length 告诉浏览器响应体有多少字节（写少了浏览器会一直等）
    std::string http_response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: " + std::to_string(html_content.size()) + "\r\n"
        "Connection: close\r\n"
        "\r\n" +            // 空行：响应头结束的标志
        html_content;

    // 把响应发回给客户端
    send(client_fd, http_response.c_str(), http_response.length(), 0);

    close(client_fd);   // 关闭连接（Connection: close 表示响应完就断开）
}

// ============================================================================
//  主函数：创建服务器并不断接受连接
//  流程：socket → setsockopt → bind → listen → while(accept) → handle_client
// ============================================================================
int main() {
    int server_fd, client_fd;         // 监听 socket 和客户端 socket
    struct sockaddr_in address;       // 地址结构（存 IP + 端口）
    int addrlen = sizeof(address);
    int opt = 1;

    // 1. 创建 TCP Socket
    // socket() 失败返回 -1；严格来说这里应该判断 < 0 而不是 == 0
    // （0 也是合法的文件描述符，这里是教学代码，行为上不影响测试）
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
    address.sin_family = AF_INET;                    // IPv4
    address.sin_addr.s_addr = INADDR_ANY;            // 监听所有网卡（0.0.0.0）
    address.sin_port = htons(PORT);                  // 端口转网络字节序

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
        // accept 阻塞等待新连接，返回后 client_fd 就是和客户端通信的 socket
        if ((client_fd = accept(server_fd, (struct sockaddr*)&address, (socklen_t*)&addrlen)) < 0) {
            perror("accept 失败");
            continue;
        }

        // 打印客户端 IP 和端口（方便观察是谁连进来的）
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &address.sin_addr, client_ip, INET_ADDRSTRLEN);
        std::cout << "[服务器] 新连接来自: " << client_ip << ":" << ntohs(address.sin_port) << std::endl;

        // 单线程处理（处理完一个才能接下一个）
        handle_client(client_fd);
    }

    close(server_fd);   // 关闭监听 socket（实际到不了这里，因为上面是死循环）
    return 0;
}
