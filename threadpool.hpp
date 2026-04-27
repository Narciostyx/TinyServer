#ifndef _THREAD_POOL_HPP
#define _THREAD_POOL_HPP

#include <vector>
#include <queue>
#include <functional>
#include <atomic>

#include "mutex.hpp"
#include "thread.hpp"
#include "log.hpp"

namespace project {
    namespace
    {
        constexpr int kDefaultThreadNum = 20;
        constexpr int kDefaultRequestNum = 100;
    }

    // 线程池类：用于执行异步任务
    class ThreadPool {
    public:
        ThreadPool(size_t thread_count = kDefaultThreadNum, size_t max_requests = kDefaultRequestNum);
        ~ThreadPool();

        // 向线程池添加任务（队列满时阻塞）
        void enqueue(std::function<void()> task);

    private:
        std::vector<Thread> workers_;                     // 工作线程
        std::queue<std::function<void()>> tasks_;         // 任务队列
        Mutex queue_mutex_;                               // 队列互斥锁
        CondVar condition_;                               // 条件变量
        CondVar not_full_;                                // 队列未满条件变量
        std::atomic<bool> stop_;                          // 停止标志

        size_t max_threads_;                             // 最大线程数
        size_t max_requests_;                            // 最大请求数

        // 工作线程主函数
        void worker();
    };
}
#endif