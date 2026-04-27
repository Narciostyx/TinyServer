#include "reactor.hpp"

#include <errno.h>
#include <stdint.h>
#include <sys/eventfd.h>

// 子Reactor构造：保存线程池指针
project::SubReactor::SubReactor(ThreadPool* pool, int max_listen) : threadpool_(pool), listen_max_(max_listen) {
    epoll_fd_ = epoll_create1(0);
    if (epoll_fd_ < 0)
        throw Err("Failed to create the file descriptor of epoll.", kErrType::Reactor_init);

    wakeup_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeup_fd_ < 0)
        throw Err("Failed to create eventfd for reactor wakeup.", kErrType::Reactor_init);

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = wakeup_fd_;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wakeup_fd_, &ev) < 0)
        throw Err("Failed to add wakeup fd to epoll.", kErrType::Reactor_init);
}

project::SubReactor::~SubReactor() {
    stop();
    if (wakeup_fd_ >= 0) close(wakeup_fd_);
    if (epoll_fd_ >= 0) close(epoll_fd_);
    for (const auto& pair : fd_contexts_) {
        close(pair.first);
    }
    if (!abnormalFd_.empty())
    {
        LOG_INFO("Start to close the abnormal file descriptors.");
        for (auto i : abnormalFd_)
            close(i);
    }
}

void project::SubReactor::stop()
{
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false))
        return;

    if (wakeup_fd_ >= 0)
    {
        uint64_t one = 1;
        (void)::write(wakeup_fd_, &one, sizeof(one));
    }
}

bool project::SubReactor::add_fd(int fd) {
    if (current_listen_ >= listen_max_) {
        LOG_WARN("SubReactor reached max listen capacity.");
        return false; // 达到上限，拒绝
    }

    epoll_event ev{};
    // 默认监听读事件和边缘触发(如果需要)，以及对端断开连接(EPOLLRDHUP | EPOLLHUP)
    ev.events = EPOLLIN | EPOLLRDHUP | EPOLLHUP; 
    ev.data.fd = fd;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0)
    {
        LOG_ERR("Failed to add the fd to epoll instance.");
        return false;
    }

    // 初始化上下文
    fd_contexts_[fd] = FdContext{};
    current_listen_++;
    return true;
}

void project::SubReactor::set_callbacks(int fd, EventCallback read_cb, EventCallback write_cb) {
    if (fd_contexts_.count(fd)) {
        fd_contexts_[fd].read_cb = std::move(read_cb);
        fd_contexts_[fd].write_cb = std::move(write_cb);
    }
}

bool project::SubReactor::modify_epoll_events(int fd, uint32_t events) {
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) < 0) {
        LOG_ERR("Failed to modify epoll events for fd " + std::to_string(fd));
        return false;
    }
    return true;
}

bool project::SubReactor::remove_fd(int fd) {
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) < 0)
    {
        LOG_ERR("Failed to remove the fd " + std::to_string(fd) + " from epoll instance and push into the abnormal queue.");
        abnormalFd_.push_back(fd);
    }
    else
        close(fd);

    if (fd_contexts_.erase(fd))
        current_listen_--;
    return true;
}

void project::SubReactor::loop() {
    const int MAX_EVENTS = kMaxListenNum;
    epoll_event events[MAX_EVENTS];
    while (running_.load(std::memory_order_relaxed)) {
        int n = epoll_wait(epoll_fd_, events, MAX_EVENTS, -1);
        if (n < 0)
        {
            if (errno == EINTR) continue;
            LOG_ERR("epoll_wait failed.");
            continue;
        }
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            uint32_t trigger_events = events[i].events;

            if (fd == wakeup_fd_)
            {
                uint64_t v = 0;
                while (::read(wakeup_fd_, &v, sizeof(v)) > 0) {}
                continue;
            }

            if (!fd_contexts_.count(fd)) {
                LOG_WARN("No context links to this fd.");
                continue;
            }

            auto& ctx = fd_contexts_[fd];
            bool should_close = false;

            // 对端关闭连接或发生错误
            if (trigger_events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                should_close = true;
            } else {
                // 读事件触发
                if ((trigger_events & EPOLLIN) && ctx.read_cb) {
                    if (!ctx.read_cb(fd)) {
                        should_close = true; // 回调返回false认为客户端要求断开
                    }
                }
                // 写事件触发（如果没有判断将被关闭）
                if (!should_close && (trigger_events & EPOLLOUT) && ctx.write_cb) {
                    if (!ctx.write_cb(fd)) {
                        should_close = true;
                    }
                }
            }

            if (should_close) {
                remove_fd(fd);
            }
        }
    }
}

// 主Reactor构造：注册监听socket的回调，分发新连接到子Reactor
project::MainReactor::MainReactor(Acceptor& acceptor,
    std::vector<std::shared_ptr<SubReactor>>& subs,
    InitCallback init_cb)
    : next_sub_(0), acceptor_(acceptor), sub_reactors_(subs), init_cb_(std::move(init_cb))
{
    epoll_fd_ = epoll_create1(0);
    if (epoll_fd_ < 0)
        throw Err("Failed to create the file descriptor of epoll.", kErrType::Reactor_init);

    wakeup_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeup_fd_ < 0)
        throw Err("Failed to create eventfd for reactor wakeup.", kErrType::Reactor_init);

    epoll_event ev_wakeup{};
    ev_wakeup.events = EPOLLIN;
    ev_wakeup.data.fd = wakeup_fd_;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wakeup_fd_, &ev_wakeup) < 0)
        throw Err("Failed to add wakeup fd to epoll.", kErrType::Reactor_init);


    epoll_event ev_accept{};
    ev_accept.events = EPOLLIN;
    ev_accept.data.fd = acceptor_.get_fd();
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, acceptor_.get_fd(), &ev_accept) < 0)
        throw Err("Failed to add acceptor fd to epoll.", kErrType::Reactor_init);

    LOG_INFO("Initialized the mainReactor.");
}

project::MainReactor::~MainReactor() {
    stop();
    if (wakeup_fd_ >= 0) close(wakeup_fd_);
    if (epoll_fd_ >= 0) close(epoll_fd_);
}

void project::MainReactor::stop()
{
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false))
        return;

    if (wakeup_fd_ >= 0)
    {
        uint64_t one = 1;
        (void)::write(wakeup_fd_, &one, sizeof(one));
    }
}

void project::MainReactor::loop() {
    const int MAX_EVENTS = kMaxListenNum;
    epoll_event events[MAX_EVENTS];
    while (running_.load(std::memory_order_relaxed)) {
        int n = epoll_wait(epoll_fd_, events, MAX_EVENTS, -1);
        if (n < 0)
        {
            if (errno == EINTR) continue;
            LOG_ERR("epoll_wait failed.");
            continue;
        }
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            if (fd == wakeup_fd_)
            {
                uint64_t v = 0;
                while (::read(wakeup_fd_, &v, sizeof(v)) > 0) {}
                continue;
            }
            else if (fd == acceptor_.get_fd())
                dispatch_(fd);
        }
    }
}