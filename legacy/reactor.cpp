#include "reactor.hpp"
#include "connection.hpp"

#include <errno.h>
#include <stdint.h>
#include <algorithm>
#include <sys/eventfd.h>

// 子Reactor构造：保存线程池指针
project::SubReactor::SubReactor(ThreadPool* pool, int max_listen,int timeout) : threadpool_(pool), listen_max_(max_listen),idle_timeout_sec_(timeout) {
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

// 跨线程投递任务到 reactor 线程（线程安全，eventfd 唤醒）
void project::SubReactor::post(std::function<void()> task)
{
    {
        std::lock_guard<std::mutex> lock(task_mu_);
        tasks_.push_back(std::move(task));
    }
    if (wakeup_fd_ >= 0)
    {
        uint64_t one = 1;
        (void)::write(wakeup_fd_, &one, sizeof(one));
    }
}

// 消费任务队列（loop 内调用，reactor 线程）
void project::SubReactor::process_tasks()
{
    std::deque<std::function<void()>> local;
    {
        std::lock_guard<std::mutex> lock(task_mu_);
        local.swap(tasks_);
    }
    for (auto& t : local)
    {
        try { t(); }
        catch (const std::exception& e) { LOG_ERR(std::string("Exception in reactor task: ") + e.what()); }
        catch (...) { LOG_ERR("Unknown exception in reactor task."); }
    }
}

bool project::SubReactor::add_fd(int fd) {
    if (current_listen_ >= listen_max_) {
        LOG_WARN("SubReactor reached max listen capacity.");
        return false; // 达到上限，拒绝
    }

    epoll_event ev{};
    // 边缘触发(EPOLLET)：读回调必须循环 recv 到 EAGAIN；配合非阻塞 fd 使用
    ev.events = EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLET;
    ev.data.fd = fd;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0)
    {
        LOG_ERR("Failed to add the fd to epoll instance.");
        return false;
    }

    // 初始化上下文
    FdContext ctx{};
    auto now = std::chrono::steady_clock::now();
    ctx.last_active = now;
    ctx.last_heap_push = now;
    fd_contexts_[fd] = std::move(ctx);
    // 新连接注册即推进一条空闲到期记录（便于空闲连接按时清理）
    idle_heap_.push(IdleEntry{ now + std::chrono::seconds(idle_timeout_sec_), fd });
    current_listen_++;
    return true;
}

void project::SubReactor::set_callbacks(int fd, EventCallback read_cb, EventCallback write_cb, std::shared_ptr<Connection> conn) {
    auto it = fd_contexts_.find(fd);
    if (it == fd_contexts_.end()) return;
    it->second.read_cb = std::move(read_cb);
    it->second.write_cb = std::move(write_cb);
    it->second.conn_ptr = std::move(conn);
    auto now = std::chrono::steady_clock::now();
    it->second.last_active = now;
    it->second.last_heap_push = now;
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
    // 幂等保护：已移除的 fd 直接返回，避免重复 DEL 误删"被内核重用的 fd"
    if (fd_contexts_.find(fd) == fd_contexts_.end())
        return true;

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

// 事件触发时更新该 fd 的活跃时间并推进最小堆
void project::SubReactor::on_active(int fd) {
    auto it = fd_contexts_.find(fd);
    if (it == fd_contexts_.end())
        return;
    auto now = std::chrono::steady_clock::now();
    it->second.last_active = now;
    // close_expired本身依据last_active，而非最小堆里的值
    if (now - it->second.last_heap_push >= std::chrono::seconds(1)) {
        it->second.last_heap_push = now;
        idle_heap_.push(IdleEntry{ now + std::chrono::seconds(idle_timeout_sec_), fd });
    }
}

// 距下一个空闲连接到期的毫秒数；无到期项返回 -1
long long project::SubReactor::next_timeout_ms() {
    while (!idle_heap_.empty()) {
        auto now = std::chrono::steady_clock::now();
        const auto& top = idle_heap_.top();
        if (top.expire_at <= now) return 0; // 已有到期项，epoll_wait 立即返回以处理
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(top.expire_at - now).count();
        return ms;
    }
    return -1;
}

// 关闭已到期的空闲连接（lazy deletion：堆顶条目对应的 fd 若期间又活跃过则跳过）
void project::SubReactor::close_expired() {
    auto now = std::chrono::steady_clock::now();
    while (!idle_heap_.empty() && idle_heap_.top().expire_at <= now) {
        IdleEntry e = idle_heap_.top();
        idle_heap_.pop();
        auto it = fd_contexts_.find(e.fd);
        if (it == fd_contexts_.end()) continue; // 连接已被移除，lazy 跳过
        if (now - it->second.last_active < std::chrono::seconds(idle_timeout_sec_)) continue; // 期间又活跃过
        LOG_WARN("fd " + std::to_string(e.fd) + " idle timeout, will close.");
        const char disconnect_msg[] = "Server forced disconnect due to inactivity\r\n";
        (void)::send(e.fd, disconnect_msg, sizeof(disconnect_msg) - 1, MSG_NOSIGNAL);
        remove_fd(e.fd);
    }
}

// 刷新连接写缓冲：发送响应；遇 EAGAIN 注册 EPOLLOUT 等待可写事件（reactor 线程内调用）
void project::SubReactor::flush_write(std::shared_ptr<Connection> conn) {
    if (!conn) return;
    int fd = conn->get_fd();
    if (fd_contexts_.find(fd) == fd_contexts_.end())
        return; // 连接已被移除，不再发送

    std::vector<char> data = conn->take_write_buffer();
    size_t off = 0;
    while (off < data.size()) {
        int n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
        if (n > 0) { off += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // 发送缓冲满：未发送部分放回缓冲，注册 EPOLLOUT 由写事件继续发送
            conn->prepend_write_buffer(data.data() + off, data.size() - off);
            modify_epoll_events(fd, EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLHUP | EPOLLET);
            return;
        }
        LOG_WARN("fd " + std::to_string(fd) + " send error, close. errno: " + std::to_string(errno));
        remove_fd(fd);
        return;
    }
}

void project::SubReactor::loop() {
    const int MAX_EVENTS = kMaxListenNum;
    epoll_event events[MAX_EVENTS];
    while (running_.load(std::memory_order_relaxed)) {
        // 先消费由 post() 投递的任务（新连接注册、发送响应、关闭连接等）
        process_tasks();

        // epoll_wait 超时与"下一个空闲连接到期时刻"联动（无到期项时兜底 1 秒）
        long long tmo = next_timeout_ms();
        int wait_ms = (tmo < 0) ? 1000 : (int)std::min<long long>(tmo, 1000);

        int n = epoll_wait(epoll_fd_, events, MAX_EVENTS, wait_ms);
        if (n < 0)
        {
            if (errno == EINTR) continue;
            LOG_ERR("epoll_wait failed.");
            continue;
        }

        // 处理已到期的空闲连接
        close_expired();

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

            // 更新活跃时间（推进空闲超时最小堆）
            ctx.last_active = std::chrono::steady_clock::now();
            on_active(fd);

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
