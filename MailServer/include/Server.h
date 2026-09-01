// ============================================================================
//  Server.h —— 服务器基类（抽象层 / 模板方法模式）
//
//  作用：
//    这是一个"通用 TCP 服务器"的抽象基类。它把网络编程里最繁琐、最通用的部分
//    （创建 socket、bind、listen、accept、多线程）全部封装好，
//    子类只需要关心"每个客户端连上来之后怎么处理"这一个问题。
//
//  使用方法（三步）：
//    class MyServer : public Server {
//    protected:
//        // 必须实现：当有客户端连接时，具体要做什么
//        void handleClient(int client_fd) override { ... }
//    };
//    MyServer server(8080);
//    server.start();
//
//  设计模式：模板方法（Template Method）
//    - 基类 Server::start() 负责固定流程（socket → bind → listen → accept 循环）
//    - 子类通过重写 handleClient() 决定每个连接的"具体行为"
//    - 这样"不变的部分"（网络框架）放在基类，"可变的部分"（业务逻辑）放在子类
// ============================================================================
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
    // 注意：这里只是把端口号存进成员变量，真正的 socket 创建在 start() 里完成
    Server(int port);

    // 虚析构函数，确保子类正确释放资源
    // 声明为 virtual 的原因：当通过基类指针删除子类对象时，
    // C++ 能正确调用到最终派生类的析构函数；否则只会调用基类析构，子类资源可能泄漏
    virtual ~Server();

    // 启动服务器，返回是否成功
    // 内部完成：创建 socket → setsockopt → bind → listen → accept 循环
    // 这是一个阻塞调用，会一直运行到 stop() 被调用（或进程被杀）
    bool start();

    // 停止服务器（优雅关闭）
    // 把 is_running 设为 false 并关闭监听 socket，
    // 这样阻塞在 accept() 的线程会立即返回失败，从而退出循环
    void stop();

protected:
    // 纯虚函数：处理一个客户端连接
    // 子类（如 SmtpServer、Pop3Server）必须实现这个函数
    // 当有新的 TCP 连接被 accept 后，start() 会为这个连接创建一个新线程，
    // 然后在新线程里调用 handleClient(client_fd)
    virtual void handleClient(int client_fd) = 0;

private:
    int server_fd;          // 监听套接字（socket 文件描述符）
                            // 它只负责"接受新连接"，本身不用于收发数据；
                            // accept 返回的 client_fd 才是跟客户端聊天的那个
    int port;               // 监听的端口号（构造函数保存，start() 里使用）
    std::atomic<bool> is_running; // 原子布尔值，控制服务器是否继续运行（线程安全）
                            // 用 std::atomic 是因为 stop() 可能从另一个线程被调用，
                            // 普通 bool 在多线程下读写属于"数据竞争"（未定义行为）

    // 禁用拷贝构造和赋值（防止意外复制）
    // 服务器对象持有 socket 和线程，一旦被复制，两个对象指向同一个资源，
    // 析构时重复释放会崩溃，所以直接禁止拷贝
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
};

#endif // SERVER_H
