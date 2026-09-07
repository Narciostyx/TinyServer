#ifndef _REACTOR_HPP
#define _REACTOR_HPP

#include <functional>
#include <unordered_map>
#include <vector>
#include <deque>
#include <queue>
#include <mutex>
#include <memory>
#include <sys/epoll.h>
#include <unistd.h>
#include <iostream>
#include <atomic>
#include <chrono>

#include "threadpool.hpp"
#include "acceptor.hpp"
#include "log.hpp"
#include "config.hpp"

namespace project {

    namespace { constexpr int kMaxListenNum = 17; }

    class ThreadPool;
    class Connection;

    /**
     * 子reactor，负责已连接的fd的IO事件，并将任务交给线程池.
     * 子reactor只负责接受来自客户端的请求，发送线程池处理完成的响应以及接收新连接、关闭连接
     * 当某一连接长时间未活跃将被关闭，取决于idle_timeout_sec_
     */
    class SubReactor {
    private:
        // 回调签名：参数是当前fd。返回bool，true表示继续处理，false表示客户端申请关闭或出现异常，子reactor将主动销毁此fd
        using EventCallback = std::function<bool(int)>;
        // 事件处理回调函数
        struct FdContext
        {
            EventCallback read_cb;
            EventCallback write_cb;
            std::shared_ptr<Connection> conn_ptr;
            std::chrono::steady_clock::time_point last_active;    // 最近活跃时间（空闲超时判定）
            std::chrono::steady_clock::time_point last_heap_push; // 上次向空闲堆 push 的时间（节流防堆膨胀）
        };
        // 定义空闲队列，到期移除
        struct IdleEntry
        {
            std::chrono::steady_clock::time_point expire_at;
            int fd;
        };
        // 最小堆：最早到期在堆顶
        struct IdleEntryCmp
        {
            bool operator()(const IdleEntry& a, const IdleEntry& b) const
            {
                return a.expire_at > b.expire_at;
            }
        };

    public:
        /**
         * 构造子Reactor：创建 epoll 实例与 eventfd 唤醒句柄
         * \param pool 线程池指针（业务任务投递目标）
         * \param max_listen 本 reactor 可承载的最大连接数
         * \param timeout 连接空闲超时秒数（超过即被清理）
         */
        SubReactor(ThreadPool* pool, int max_listen, int timeout);
        /**
         * 析构：停止事件循环，关闭 wakeup/epoll 及所有托管 fd
         */
        virtual ~SubReactor();

        // 禁止拷贝
        SubReactor(const SubReactor&) = delete;
        SubReactor& operator=(const SubReactor&) = delete;

        /**
         * 注册新连接的 FD 到 epoll（边缘触发），并建立该 fd 的回调上下文
         * \param fd 新连接的文件描述符
         * \return true 注册成功；false 连接数达上限或 epoll 注册失败
         */
        bool add_fd(int fd);

        /**
         * 删除 fd 并释放其资源（幂等，仅 reactor 线程内调用）
         * \param fd 要移除的文件描述符
         * \return true（接口幂等，恒返回成功）
         */
        bool remove_fd(int fd);

        /**
         * 为给定 fd 注册读写回调，同时记录 connection 用于超时管理
         * \param fd 目标文件描述符
         * \param read_cb 读事件回调（返回 false 表示连接应关闭）
         * \param write_cb 写事件回调（返回 false 表示连接应关闭）
         * \param conn 关联的连接对象（默认空）
         */
        void set_callbacks(int fd, EventCallback read_cb, EventCallback write_cb, std::shared_ptr<Connection> conn = nullptr);

        /**
         * 修改某 fd 在 epoll 上监听的事件（如动态增删 EPOLLOUT）
         * \param fd 目标文件描述符
         * \param events 目标事件掩码（EPOLLIN / EPOLLOUT 等）
         * \return true 修改成功；false 修改失败
         */
        bool modify_epoll_events(int fd, uint32_t events);

        /**
         * 事件主循环：消费 post 任务，epoll_wait 等待 IO/超时事件并分发给回调
         */
        void loop();
        /**
         * 请求退出事件循环：置停止标志并写入 eventfd 唤醒阻塞的 epoll_wait
         */
        void stop();

        /**
         * 跨线程投递任务到 reactor 线程执行（线程安全，eventfd 唤醒）
         * \param task 将在 reactor 线程执行的任务
         */
        void post(std::function<void()> task);

        /**
         * 刷新连接写缓冲：发送响应；遇 EAGAIN 注册 EPOLLOUT，由后续写事件继续发送
         * \param conn 待发送响应的连接（reactor 线程内调用）
         */
        void flush_write(std::shared_ptr<Connection> conn);

    // 内部数据结构和方法
    private:
        int epoll_fd_ = -1;
        int wakeup_fd_ = -1;
        int listen_max_ = 10000;
        int current_listen_ = 0;
        std::atomic<bool> running_{ true };

        // 空闲连接超时阈值（秒）
        long long idle_timeout_sec_ = 60;

        // 映射文件描述符与之关联的回调函数结构体
        std::unordered_map<int, FdContext> fd_contexts_;
        // 处于异常状态的文件描述符
        std::vector<int> abnormalFd_;
        ThreadPool* threadpool_; // 线程池指针

        // 跨线程任务队列
        std::mutex task_mu_;
        std::deque<std::function<void()>> tasks_;
        std::priority_queue<IdleEntry, std::vector<IdleEntry>, IdleEntryCmp> idle_heap_;

        /**
         * 消费任务队列中由 post() 投递的任务（loop 内调用，reactor 线程）
         */
        void process_tasks();
        /**
         * 事件触发时更新该 fd 的活跃时间并推进最小堆（1 秒节流，防堆条目膨胀）
         * \param fd 活跃的文件描述符
         */
        void on_active(int fd);
        /**
         * 计算距下一个空闲连接到期的时间
         * \return 距堆顶到期的毫秒数；无到期项返回 -1
         */
        long long next_timeout_ms();
        /**
         * 关闭已到期的空闲连接（lazy deletion：堆顶条目对应 fd 若期间又活跃过则跳过）
         */
        void close_expired();
    };

    /**
     * 主Reactor：负责监听端口，把新连接分发（post）给某个子Reactor
     * 保证 fd_contexts_ 只在子 reactor 线程内被访问
     */
    class MainReactor {
    public:
        /**
         * 新连接初始化钩子类型：为子Reactor 中的新 fd 注入读写回调
         * \param reactor 目标子Reactor
         * \param fd 新连接的文件描述符
         */
        using InitCallback = std::function<void(SubReactor*, int)>;

        /**
         * 构造主Reactor：注册监听 socket 与 eventfd 到 epoll
         * \param acceptor 监听器引用（提供 accept 与监听 fd）
         * \param sub_reactors 子Reactor 列表引用（轮询分发目标）
         * \param init_cb 新连接初始化钩子（默认空）
         */
        MainReactor(Acceptor& acceptor,
                    std::vector<std::shared_ptr<SubReactor>>& sub_reactors,
                    InitCallback init_cb = nullptr);
        /**
         * 析构：停止事件循环并关闭 wakeup/epoll 句柄
         */
        virtual ~MainReactor();

        /**
         * 主事件循环：epoll_wait 监听 accept 事件并分发新连接（主线程运行）
         */
        void loop();
        /**
         * 请求退出事件循环：置停止标志并写入 eventfd 唤醒 epoll_wait
         */
        void stop();

    private:
        int epoll_fd_ = -1;
        int wakeup_fd_ = -1;
        std::atomic<bool> running_{ true };

        int next_sub_;    // 轮询分配子Reactor
        Acceptor& acceptor_;
        std::vector<std::shared_ptr<SubReactor>>& sub_reactors_;
        InitCallback init_cb_;

        // 分发：把 accept 得到的 fd 通过 post 投递给子 reactor 线程注册，
        // 保证 fd_contexts_ 只在子 reactor 线程内访问（消除主线程/子线程并发访问哈希表的数据竞争）
        std::function<void(int)> dispatch_ = [this](int fd)->void
            {
                int client_fd = acceptor_.accept();
                if (client_fd < 0) return;
                if (sub_reactors_.empty()) {
                    LOG_WARN("No subreactor associates with the mainreactor(the subreactor is empty).");
                    ::close(client_fd);
                    return;
                }
                int idx = next_sub_++ % sub_reactors_.size();
                auto sr = sub_reactors_[idx];
                auto cb = init_cb_;
                sr->post([sr, client_fd, cb]() {
                    if (!sr->add_fd(client_fd)) {
                        LOG_WARN("Max conncetions reached, refuse the connect request.");
                        ::close(client_fd);
                        return;
                    }
                    // 调用初始化钩子为该文件描述符赋予业务回调（由Server实现控制）
                    if (cb) cb(sr.get(), client_fd);
                });
            };
    };

}
#endif
