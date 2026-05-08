#ifndef _REACTOR_HPP
#define _REACTOR_HPP

#include <functional>
#include <unordered_map>
#include <vector>
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

    // 子Reactor：负责已连接fd的IO事件，事件到来时将任务交给线程池
    class SubReactor {
    public:
        // 回调签名。参数是当前fd。返回bool，true表示继续处理，false表示客户端申请关闭或出现异常，子reactor将主动销毁此fd
        using EventCallback = std::function<bool(int)>;

        SubReactor(ThreadPool* pool, int max_listen, int timeout);
        virtual ~SubReactor();

        // 禁用拷贝机制，保护底层 fd 不被错误共享和重复清理
        SubReactor(const SubReactor&) = delete;
        SubReactor& operator=(const SubReactor&) = delete;

        // 供MainReactor调用，传入新连接的 FD
        bool add_fd(int fd);

        // 删除某个 fd 并释放资源
        bool remove_fd(int fd);

        // 为给定的 fd 注册读写回调，同时记录 connection 用于超时管理
        void set_callbacks(int fd, EventCallback read_cb, EventCallback write_cb, std::shared_ptr<Connection> conn = nullptr);

        // 修改感兴趣的事件（例如关注可写事件）
        bool modify_epoll_events(int fd, uint32_t events);

        // 事件主循环
        void loop();
        // 请求退出事件循环
        void stop();
        // 等待关闭的连接的超时检查
        void check_idle_connections();
    private:
        // 内部数据结构和方法
        int epoll_fd_ = -1;
        int wakeup_fd_ = -1;
        int listen_max_ = 10000;
        int current_listen_ = 0;
        std::atomic<bool> running_{ true };
        int time_out_;

        long long idle_timeout_s_{ 60 };
        std::chrono::steady_clock::time_point last_check_time_;

        //事件处理回调函数
        struct FdContext {
            EventCallback read_cb;
            EventCallback write_cb;
            std::shared_ptr<Connection> conn_ptr;
        };
        //映射文件描述符与之关联的回调函数结构体
        std::unordered_map<int, FdContext> fd_contexts_;
        //处于异常状态的文件描述符
        std::vector<int> abnormalFd_;
        ThreadPool* threadpool_; // 线程池指针
    };

    // 主Reactor：负责监听端口，分发新连接到子Reactor
    class MainReactor {
    public:
        // 现在构造函数支持接受一个供所有新连接使用的钩子函数，用于统一注入读写回调
        using InitCallback = std::function<void(SubReactor*, int)>;

        MainReactor(Acceptor& acceptor,
                    std::vector<std::shared_ptr<SubReactor>>& sub_reactors,
                    InitCallback init_cb = nullptr);
        virtual ~MainReactor();

        void loop();
        void stop();

    private:
        int epoll_fd_ = -1;
        int wakeup_fd_ = -1;
        std::atomic<bool> running_{ true };

        int next_sub_;    // 轮询分配子Reactor
        Acceptor& acceptor_;
        std::vector<std::shared_ptr<SubReactor>>& sub_reactors_;
        InitCallback init_cb_;
        std::function<void(int)> dispatch_ = [this](int fd)->void
            {
                int client_fd = acceptor_.accept();
                if (client_fd >= 0) {
                    if (!sub_reactors_.empty()) {
                        int idx = next_sub_++ % sub_reactors_.size();
                        auto& sr = sub_reactors_[idx];
                        if (sr->add_fd(client_fd)) {
                            // 调用初始化钩子为该文件描述符赋予业务回调（由Server实现控制）
                            if (init_cb_) {
                                init_cb_(sr.get(), client_fd);
                            }
                        }
                        else {
                            // SubReactor可能超过最大listen_max拒绝了该请求
                            LOG_WARN("Max conncetions reached, refuse the connect request.");
                            close(client_fd);
                        }
                    } else {
                        // 回退机制：若无子reactor直接关闭
                        LOG_WARN("No subreactor associates with the mainreactor(the subreactor is empty).");
                        close(client_fd);
                    }
                }
            };
    };

}
#endif