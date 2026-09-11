#ifndef SERVER_H
#define SERVER_H

#include <string>
#include <atomic>
#include <thread>
#include <vector>
#include <memory>
#include "ThreadPool.h"


class Server {
public:

    Server(int port);

    virtual ~Server();

    bool start();

    void stop();

protected:

    virtual void handleClient(int client_fd) = 0;

private:
    int server_fd;
    int port;
    std::atomic<bool> is_running;
    std::unique_ptr<ThreadPool> pool_; 
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
};

#endif // SERVER_H
