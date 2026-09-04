#ifndef SERVER_H
#define SERVER_H

#include <string>
#include <atomic>
#include <thread>
#include <vector>


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
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
};

#endif // SERVER_H
