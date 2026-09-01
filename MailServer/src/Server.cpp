// ============================================================================
//  Server.cpp —— 服务器基类的实现（网络层：socket 全流程）
//
//  本文件把"通用 TCP 服务器"的生命周期完整实现：
//    socket()     创建套接字
//    setsockopt() 设置端口复用
//    bind()       绑定 IP 和端口
//    listen()     开始监听
//    accept()     循环接受新连接（每连接开一个线程）
//  具体每个客户端的业务逻辑由子类的 handleClient() 决定（模板方法模式）
// ============================================================================
#include "Server.h"
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

// ============================================================================
//  构造函数
//  注意：初始化顺序必须与头文件中的声明顺序一致（server_fd, port, is_running）
//  C++ 规定成员变量按"声明顺序"初始化（而不是按初始化列表的书写顺序），
//  顺序不一致会触发编译警告，这里严格保持一致
// ============================================================================
Server::Server(int port)
    : server_fd(-1), port(port), is_running(false) {
    // server_fd = -1 表示"还没有创建 socket"（-1 是无效的文件描述符）
    // is_running = false 表示"尚未开始运行"
}

// ============================================================================
//  析构函数：确保服务器停止并释放资源
//  stop() 会关闭监听 socket；如果服务器正在运行，还能让 accept 循环退出
// ============================================================================
Server::~Server() {
    stop();
}

// ============================================================================
//  启动服务器（核心：TCP 服务器标准五步曲）
// ============================================================================
bool Server::start() {
    // ---------- 第 1 步：创建 TCP socket ----------
    // AF_INET     : IPv4 协议族
    // SOCK_STREAM : 流式套接字（TCP：面向连接、可靠、按字节流传输）
    // 0           : 让系统根据前两个参数自动选择协议（TCP）
    // 返回值 server_fd 是一个"文件描述符"（非负整数），
    // 之后所有系统调用（bind/listen/accept/close）都靠它来引用这个套接字
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket 创建失败");   // perror 打印错误描述到 stderr
        return false;
    }

    // ---------- 第 2 步：设置端口复用（SO_REUSEADDR）----------
    // 如果不设置：程序异常退出后，端口会进入 TIME_WAIT 状态，
    // 短时间内重新启动会报 "Address already in use"（端口被占用）
    // 设置之后，可以立即重启复用同一个端口
    int opt = 1;   // 1 = 开启该选项
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt 失败");
        close(server_fd);
        return false;
    }

    // ---------- 第 3 步：绑定地址和端口 ----------
    // sockaddr_in 是 IPv4 的地址结构体，里面存"IP + 端口"
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));   // 先清零，避免残留垃圾数据
    address.sin_family = AF_INET;           // 地址族：IPv4
    address.sin_addr.s_addr = INADDR_ANY;   // 监听所有网卡（0.0.0.0），
                                            // 这样局域网里的其他设备也能连进来
    address.sin_port = htons(port);         // 端口转为网络字节序
                                            // htons = Host TO Network Short：
                                            // x86 是小端机，网络字节序是大端，必须转换

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind 失败（端口被占用？）");
        close(server_fd);
        return false;
    }

    // ---------- 第 4 步：开始监听，最大等待队列为 5 ----------
    // listen() 让内核开始接受连接；第二个参数 5 是"连接等待队列长度"：
    // 如果同时有超过 5 个客户端在排队等 accept，新的连接会被内核拒绝
    if (listen(server_fd, 5) < 0) {
        perror("listen 失败");
        close(server_fd);
        return false;
    }

    // 服务器启动成功，进入主循环
    is_running = true;
    std::cout << "[服务器] 已启动，监听端口 " << port << std::endl;

    // ---------- 第 5 步：主循环：不断接受新连接 ----------
    while (is_running) {
        struct sockaddr_in client_addr;   // 用来接收"谁连进来了"的信息
        socklen_t client_len = sizeof(client_addr);

        // accept 阻塞等待新连接：
        //   没有客户端连接时，这一行会一直挂起等待；
        //   有客户端连接时，返回一个新的 socket 文件描述符 client_fd，
        //   之后就用 client_fd 跟这个客户端收发数据。
        // 注意区分两个 socket：
        //   server_fd 只负责"接客"（accept 新连接），不传输数据
        //   client_fd 负责跟某一个具体客户端"聊天"
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            // 如果服务器被停止，accept 会失败，这里检查 is_running 来判断是否正常退出
            if (!is_running) {
                break;   // 正常退出循环
            }
            perror("accept 失败");
            continue;    // 非致命错误，继续等待下一个连接
        }

        // 打印客户端 IP 和端口（用于调试）
        // inet_ntop 把"二进制 IP"转成可读的 "a.b.c.d" 字符串
        // ntohs 把网络字节序的端口转回主机字节序
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        std::cout << "[服务器] 新连接来自 " << client_ip << ":"
                  << ntohs(client_addr.sin_port) << std::endl;

        // ！！！多线程处理：为每个客户端创建一个新线程，并调用 handleClient
        // 注意：这里直接创建 std::thread，并 detach 让线程独立运行
        // 更高级的做法是使用线程池，这里为了简单易懂，我们采用每连接一线程
        std::thread worker([this, client_fd]() {
            // [this, client_fd] 是 lambda 捕获列表：
            //   this      提供当前对象，用来调用虚函数 handleClient（多态派发）
            //   client_fd 是 accept 返回的客户端 socket
            // 调用子类实现的 handleClient 处理业务逻辑
            this->handleClient(client_fd);
            // 处理完成后，关闭客户端的 socket（子类也可以关闭，这里作为保险）
            close(client_fd);
        });
        worker.detach();   // 分离线程，主线程不再等待它
        // 为什么必须开线程：
        //   handleClient 是阻塞式的（要跟客户端一问一答）。
        //   如果不开线程，一个客户端在聊天时，其他客户端就永远等不到响应了
    }

    // 退出循环，关闭监听套接字
    close(server_fd);
    std::cout << "[服务器] 已停止" << std::endl;
    return true;
}

// ============================================================================
//  停止服务器
//  原理：accept 正阻塞等待连接时，close 掉 server_fd 会让它立即返回 -1，
//  主循环检测到 is_running == false 就会 break 退出
// ============================================================================
void Server::stop() {
    if (is_running) {
        is_running = false;
        // 为了快速退出 accept 阻塞，可以在这里关闭 server_fd
        // 这样 accept 会立即返回 -1，循环就会退出
        if (server_fd != -1) {
            close(server_fd);
            server_fd = -1;   // 避免重复关闭（close 已关闭的 fd 是错误操作）
        }
    }
}
