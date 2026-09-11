#ifndef MAILFORGE_THREAD_POOL_H
#define MAILFORGE_THREAD_POOL_H

// ============================================================================
//  ThreadPool.h —— 固定大小线程池（C++17，header-only）
//
//  用途：
//    1) Server 基类：把每个连接创建/销毁一个线程，改为线程池复用 worker。
//    2) /api/benchmark：用客户端线程池并发发送 100 封邮件，测试并发优势。
//
//  线程数：
//    默认取 std::thread::hardware_concurrency()；2 核服务器即 2 个 worker。
//    也可用环境变量覆盖，例如：MAILFORGE_THREADS=4 ./mail_server
//    为避免 2G 内存机器线程过多，硬上限为 32。
// ============================================================================

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

class ThreadPool {
public:
    // threads == 0 表示自动：优先读 MAILFORGE_THREADS，否则用 CPU 核数。
    explicit ThreadPool(std::size_t threads = 0) : stopping_(false), active_(0) {
        if (threads == 0) threads = DefaultThreads();
        if (threads == 0) threads = 2;
        threads = std::min<std::size_t>(threads, 32);
        workers_.reserve(threads);
        for (std::size_t i = 0; i < threads; ++i) {
            workers_.emplace_back([this]() { workerLoop(); });
        }
    }

    ~ThreadPool() { stop(); }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    template <typename F>
    void enqueue(F&& task) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) throw std::runtime_error("ThreadPool is stopping");
            tasks_.emplace(std::forward<F>(task));
        }
        cv_.notify_one();
    }

    // 等待当前已入队任务全部执行完；可多次提交/等待。
    void waitAll() {
        std::unique_lock<std::mutex> lock(mutex_);
        doneCv_.wait(lock, [this]() {
            return tasks_.empty() && active_ == 0;
        });
    }

    // 停止：已入队任务会继续执行完，然后回收所有 worker。
    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) return;
            stopping_ = true;
        }
        cv_.notify_all();
        for (std::thread& t : workers_) {
            if (t.joinable()) t.join();
        }
        workers_.clear();
    }

    std::size_t workerCount() const { return workers_.size(); }

    static std::size_t DefaultThreads() {
        if (const char* env = std::getenv("MAILFORGE_THREADS")) {
            char* end = nullptr;
            long v = std::strtol(env, &end, 10);
            if (end != env && v > 0) {
                return static_cast<std::size_t>(std::min<long>(v, 32));
            }
        }
        unsigned int hw = std::thread::hardware_concurrency();
        if (hw == 0) hw = 2;
        return static_cast<std::size_t>(std::min<unsigned int>(hw, 32));
    }

private:
    void workerLoop() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this]() {
                    return stopping_ || !tasks_.empty();
                });
                if (stopping_ && tasks_.empty()) return;
                task = std::move(tasks_.front());
                tasks_.pop();
                ++active_;
            }

            try {
                task();
            } catch (...) {
                // 单连接/单封邮件失败不能拖垮整个线程池。
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);
                --active_;
                if (tasks_.empty() && active_ == 0) {
                    doneCv_.notify_all();
                }
            }
        }
    }

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable doneCv_;
    bool stopping_;
    std::size_t active_;
};

#endif // MAILFORGE_THREAD_POOL_H