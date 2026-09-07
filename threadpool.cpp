#include "threadpool.hpp"

#include <unistd.h>

#include <thread>
#include <sstream>

// 构造函数：启动指定数量的工作线程，设置最大线程数和最大请求数
project::ThreadPool::ThreadPool(size_t thread_count, size_t max_requests)
    : max_threads_(thread_count), max_requests_(max_requests) {
    LOG_INFO("Start to initialize the threadpool.");
    for (size_t i = 0; i < max_threads_; ++i)
        workers_.emplace_back([this] { this->worker(); });
    LOG_INFO("Finished to initialize the threadpool.");
}

// 析构函数：通知所有线程退出并等待
project::ThreadPool::~ThreadPool() {
    LOG_INFO("Start to destroy the threadpool.");

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        stop_ = true;
        LOG_INFO("Main threadpool thread has notified others.");
    }
    // 锁外唤醒：既有等任务的 worker，也有等"队列非满"的 enqueue 调用方
    condition_.notify_all();
    not_full_.notify_all();

    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
    LOG_INFO("Finish to destroy the threadpool.");
}

// 添加任务到队列（队列满时阻塞等待队列非满；停止后丢弃）
void project::ThreadPool::enqueue(std::function<void()> task) {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    not_full_.wait(lock, [this] { return stop_ || tasks_.size() < max_requests_; });
    if (stop_)
    {
        LOG_WARN("Due to the stop flag, refuse the enqueue request in the threadpool.");
        return;
    }
    tasks_.push(std::move(task));
    lock.unlock();
    condition_.notify_one();
}

// 添加任务到队列（非阻塞）：队列满或已停止时返回 false
bool project::ThreadPool::try_enqueue(std::function<void()> task) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (stop_ || tasks_.size() >= max_requests_)
        return false;
    tasks_.push(std::move(task));
    condition_.notify_one();
    return true;
}

void project::ThreadPool::worker() {
    std::ostringstream oss;
    oss << ::syscall(SYS_gettid);
    auto thread_id = oss.str();
    LOG_DEBUG("Threadpool worker started in thread " + thread_id + ".");
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            condition_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
            if (stop_ && tasks_.empty())
                break; // 停止且无残留任务：退出
            task = std::move(tasks_.front());
            tasks_.pop();
            not_full_.notify_one();
        }
        try
        {
            task();
        }
        catch (const std::exception& e)
        {
            LOG_ERR(std::string("Worker task exception in thread ") + thread_id + ":" + e.what());
        }
        catch (...)
        {
            LOG_ERR("Unknown exception type caught in worker, maybe the c++ native error?");
        }
    }
    LOG_DEBUG("Threadpool worker exits in thread " + thread_id + ".");
}
