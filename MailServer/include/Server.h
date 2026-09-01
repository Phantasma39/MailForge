//用于实现基础的客户端和服务端之间的收发消息
#ifndef SERVER_H
#define SERVER_H

#include <string>
#include <atomic>
#include <thread>
#include <vector>

// 前置声明：为了管理线程，我们可能会用到线程池，这里先简单实现每连接一线程
class Server {
public:
    // 构造函数：传入要监听的端口号
    Server(int port);
    
    // 虚析构函数，确保子类正确释放资源
    virtual ~Server();

    // 启动服务器，返回是否成功
    bool start();

    // 停止服务器（优雅关闭）
    void stop();

protected:
    // 纯虚函数：处理一个客户端连接
    // 子类（如 SmtpServer、Pop3Server）必须实现这个函数
    virtual void handleClient(int client_fd) = 0;

private:
    int server_fd;          // 监听套接字
    int port;               // 监听的端口
    std::atomic<bool> is_running; // 原子布尔值，控制服务器是否继续运行（线程安全）
    
    // 禁用拷贝构造和赋值（防止意外复制）
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
};

#endif // SERVER_H