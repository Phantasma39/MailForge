#include "Server.h"
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

std::mutex Server::registryMutex_;
std::vector<Server*> Server::instances_;
std::atomic<int> Server::globalWorkerCount_{0};

Server::Server(int port)
    : server_fd(-1), port(port), is_running(false), workerCount_(0) {
    std::lock_guard<std::mutex> lock(registryMutex_);
    instances_.push_back(this);
}

Server::~Server() {
    stop();
    std::lock_guard<std::mutex> lock(registryMutex_);
    for (auto it = instances_.begin(); it != instances_.end(); ++it) {
        if (*it == this) {
            instances_.erase(it);
            break;
        }
    }
}

bool Server::start() {
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket创建失败");
        return false;
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt 失败");
        close(server_fd);
        return false;
    }

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind 失败（端口被占用？）");
        close(server_fd);
        return false;
    }

    if (listen(server_fd, 64) < 0) {
        perror("listen 失败");
        close(server_fd);
        return false;
    }

    is_running = true;
    {
        std::lock_guard<std::mutex> lock(poolMutex_);
        if (workerCount_ <= 0) workerCount_ = static_cast<int>(ThreadPool::DefaultThreads());
        pool_ = std::make_unique<ThreadPool>(static_cast<std::size_t>(workerCount_));
        globalWorkerCount_.store(workerCount_);
        std::cout << "[服务器] 已启动，监听端口 " << port
                  << "，线程池 worker 数 " << pool_->workerCount() << std::endl;
    }

    while (is_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            if (!is_running) break;
            perror("accept 失败");
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        std::cout << "[服务器] 新连接来自 " << client_ip << ":"
                  << ntohs(client_addr.sin_port) << std::endl;
        enqueueClient(client_fd);
    }

    {
        std::lock_guard<std::mutex> lock(poolMutex_);
        if (pool_) {
            pool_->stop();
            pool_.reset();
        }
    }
    close(server_fd);
    std::cout << "[服务器] 已停止" << std::endl;
    return true;
}

void Server::enqueueClient(int client_fd) {
    std::lock_guard<std::mutex> lock(poolMutex_);
    if (!pool_) {
        close(client_fd);
        return;
    }
    pool_->enqueue([this, client_fd]() {
        this->handleClient(client_fd);
        close(client_fd);
    });
}

void Server::setWorkerCount(int n) {
    if (n < 1) n = 1;
    if (n > 32) n = 32;

    std::unique_ptr<ThreadPool> oldPool;
    {
        std::lock_guard<std::mutex> lock(poolMutex_);
        workerCount_ = n;
        oldPool = std::move(pool_);
        pool_ = std::make_unique<ThreadPool>(static_cast<std::size_t>(n));
        globalWorkerCount_.store(n);
        std::cout << "[服务器] 线程池 worker 数已调整为 " << n << std::endl;
    }
    // 旧线程池不能由当前 worker 自己 join，否则会死锁。
    // 放到独立清理线程中析构：当前请求先返回，旧 worker 随后自然退出。
    if (oldPool) {
        std::thread([pool = std::move(oldPool)]() mutable {
            pool.reset();
        }).detach();
    }
}

bool Server::setAllWorkerCounts(int n) {
    if (n < 1 || n > 32) return false;
    std::lock_guard<std::mutex> lock(registryMutex_);
    for (Server* server : instances_) {
        if (server) server->setWorkerCount(n);
    }
    return true;
}

int Server::currentWorkerCount() {
    return globalWorkerCount_.load();
}

void Server::stop() {
    if (is_running) {
        is_running = false;
        if (server_fd != -1) {
            close(server_fd);
            server_fd = -1;
        }
    }
}