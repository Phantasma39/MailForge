#ifndef SERVER_H
#define SERVER_H

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "ThreadPool.h"

class Server {
public:
    explicit Server(int port);
    virtual ~Server();

    bool start();
    void stop();

    // 管理员界面使用：动态调整所有 Server 实例的线程池大小。
    static bool setAllWorkerCounts(int n);
    static int currentWorkerCount();

protected:
    virtual void handleClient(int client_fd) = 0;

private:
    void enqueueClient(int client_fd);
    void setWorkerCount(int n);

    int server_fd;
    int port;
    std::atomic<bool> is_running;
    std::unique_ptr<ThreadPool> pool_;
    std::mutex poolMutex_;
    int workerCount_;

    static std::mutex registryMutex_;
    static std::vector<Server*> instances_;
    static std::atomic<int> globalWorkerCount_;

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
};

#endif // SERVER_H