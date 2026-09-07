#ifndef _THREAD_POOL_HPP
#define _THREAD_POOL_HPP

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>

#include "log.hpp"

namespace project {
    namespace
    {
        constexpr int kDefaultThreadNum = 20;
        constexpr int kDefaultRequestNum = 100;
    }

    /**
     * 线程池类：用于执行异步任务
     * 基于 std::thread + std::mutex + std::condition_variable
     */
    class ThreadPool {
    public:
        /**
         * 构造线程池：启动指定数量的工作线程并初始化任务队列
         * \param thread_count 工作线程数（默认 kDefaultThreadNum）
         * \param max_requests 任务队列容量上限（默认 kDefaultRequestNum）
         */
        ThreadPool(size_t thread_count = kDefaultThreadNum, size_t max_requests = kDefaultRequestNum);
        /**
         * 析构：置停止标志并唤醒所有线程，join 等待 worker 执行完残留任务后退出
         */
        ~ThreadPool();

        /**
         * 添加任务（队列满时阻塞等待队列非满；已停止则丢弃任务）
         * \param task 待执行的任务闭包
         */
        void enqueue(std::function<void()> task);

        /**
         * 添加任务（非阻塞）：队列满或已停止时立即返回失败
         * 供 Reactor 线程使用——不能让它阻塞在队列上拖死事件循环
         * \param task 待执行的任务闭包
         * \return true 入队成功；false 队列满或线程池已停止
         */
        bool try_enqueue(std::function<void()> task);

    private:
        std::vector<std::thread> workers_;                // 工作线程
        std::queue<std::function<void()>> tasks_;         // 任务队列
        std::mutex queue_mutex_;                          // 队列互斥锁
        std::condition_variable condition_;               // 有新任务（唤醒 worker）
        std::condition_variable not_full_;                // 队列未满（唤醒阻塞中的 enqueue）
        std::atomic<bool> stop_{ false };                 // 停止标志

        size_t max_threads_;                             // 最大线程数
        size_t max_requests_;                            // 最大请求数

        /**
         * 工作线程主循环：等待任务并执行，异常在任务边界被捕获
         * 停止且队列为空时退出；停止但队列有残留任务时仍执行完
         */
        void worker(); // 工作线程
    };
}
#endif
