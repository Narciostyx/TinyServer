#include "threadpool.hpp"

// 构造函数：启动指定数量的工作线程，设置最大线程数和最大请求数
project::ThreadPool::ThreadPool(size_t thread_count, size_t max_requests)
    : stop_(false), max_threads_(thread_count), max_requests_(max_requests) {
    LOG_INFO("Start to initialize the threadpool.");
    for (size_t i = 0; i < max_threads_; ++i) {
        Thread t;
        t.start([this] { this->worker(); });
        workers_.push_back(std::move(t));
    }
}

// 析构函数：通知所有线程退出并等待
project::ThreadPool::~ThreadPool() {
    LOG_INFO("Start to destroy the threadpool.");

    {
        LockGuard<> lock(queue_mutex_);
        stop_ = true;
    }

    condition_.broadcast();
    not_full_.broadcast();
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
    LOG_INFO("Finish to destroy the threadpool.");
}

// 添加任务到队列（队列满时阻塞等待）
void project::ThreadPool::enqueue(std::function<void()> task) {
    queue_mutex_.lock();
    while (!stop_ && tasks_.size() >= max_requests_)
        not_full_.wait(queue_mutex_.get());
    if (stop_)
    {
        LOG_WARN("Due to the stop flag, refuse the enqueue request in the threadpool.");
        queue_mutex_.unlock();
        return;
    }
    tasks_.push(std::move(task));
    queue_mutex_.unlock();
    condition_.signal();
}

// 工作线程主循环
void project::ThreadPool::worker() {
    while (!stop_) {
        std::function<void()> task;
        queue_mutex_.lock();
        while (!stop_ && tasks_.empty())
            condition_.wait(queue_mutex_.get());
        if (stop_ && tasks_.empty())
        {
            queue_mutex_.unlock();
            return;
        }
        task = std::move(tasks_.front());
        tasks_.pop();
        not_full_.signal();
        queue_mutex_.unlock();
        task();
    }
}