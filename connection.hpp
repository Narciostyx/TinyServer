#ifndef _CONNECTION_HPP
#define _CONNECTION_HPP

#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include <memory>
#include "http.hpp"

namespace project {

class SubReactor;
/**
 * 封装文件描述符的连接类，包含一个读缓冲区与写缓冲区.
 * 读缓冲区与写缓冲区均通过内部互斥锁保护
 */
class Connection{
public:
    /**
     * 构造连接：绑定文件描述符并预分配读写缓冲
     * \param fd 该连接对应的文件描述符
     */
    Connection(int fd) : fd_(fd) {
        read_buffer_.reserve(8192);
        write_buffer_.reserve(8192);
    }

    /**
     * 析构：默认（fd 的关闭由所属 reactor 统一负责，不在此处 close）
     */
    ~Connection() = default;

    /**
     * 获取连接绑定的文件描述符
     * \return fd（int）
     */
    int get_fd() const { return fd_; }

    /**
     * 增添读缓冲区数据（reactor 线程调用，内部加锁）
     * \param data 收到的原始字节
     * \param len 数据长度
     */
    void append_read_data(const char* data, size_t len) {
        std::lock_guard<std::mutex> lock(buf_mu_);
        read_buffer_.insert(read_buffer_.end(), data, data + len);
    }

    /**
     * 以字符串形式取出并清空读缓冲（worker 线程解析时调用，内部加锁）
     * \return 读缓冲区中的全部数据
     */
    std::string take_read_buffer() {
        std::lock_guard<std::mutex> lock(buf_mu_);
        std::string res(read_buffer_.begin(), read_buffer_.end());
        read_buffer_.clear();
        return res;
    }

    /**
     * 半包处理：解析发现数据不完整时，把已取出的数据放回缓冲头部等待更多数据
     * \param data 待放回的未完成请求数据
     */
    void prepend_read_data(const std::string& data) {
        if (data.empty()) return;
        std::lock_guard<std::mutex> lock(buf_mu_);
        read_buffer_.insert(read_buffer_.begin(), data.begin(), data.end());
    }

    /**
     * 读缓冲是否还有未处理数据
     * \return true 有未处理数据
     */
    bool has_pending() const {
        std::lock_guard<std::mutex> lock(buf_mu_);
        return !read_buffer_.empty();
    }

    /**
     * 增添写缓冲区数据（worker 线程序列化响应后调用，内部加锁）
     * \param data 待发送的响应字节
     */
    void append_write_data(const std::string& data) {
        if (data.empty()) return;
        std::lock_guard<std::mutex> lock(buf_mu_);
        write_buffer_.insert(write_buffer_.end(), data.begin(), data.end());
    }

    /**
     * 摘取全部待发送数据并清空写缓冲（reactor 线程发送时调用）
     * \return 待发送数据
     */
    std::vector<char> take_write_buffer() {
        std::lock_guard<std::mutex> lock(buf_mu_);
        std::vector<char> res = std::move(write_buffer_);
        write_buffer_.clear();
        return res;
    }

    /**
     * 发送遇到 EAGAIN 时，把未发送部分放回缓冲头部
     * \param data 未发送的数据
     * \param len 未发送的数据长度
     */
    void prepend_write_buffer(const char* data, size_t len) {
        if (len == 0) return;
        std::lock_guard<std::mutex> lock(buf_mu_);
        write_buffer_.insert(write_buffer_.begin(), data, data + len);
    }

    /**
     * 尝试占用"处理任务在飞"标志
     * \return true 表示此前无任务在飞且占用成功，可以投递新任务；false 表示已有任务在飞
     */
    bool try_acquire_task() {
        bool expect = false;
        return task_in_flight_.compare_exchange_strong(expect, true, std::memory_order_acq_rel);
    }
    /**
     * 释放"处理任务在飞"标志（连接级串行化的收尾动作）
     */
    void release_task() { task_in_flight_.store(false, std::memory_order_release); }

    /**
     * 标记"任务在飞期间又有新数据到达"（ET 下该 EPOLLIN 已消费，需任务完成后由 reactor 补投递）
     */
    void note_busy_data() { data_while_busy_.store(true, std::memory_order_relaxed); }
    /**
     * 取出并清除"任务在飞期间有新数据"标志
     * \return true 表示期间有新数据到达，应补投递下一个处理任务
     */
    bool consume_busy_flag() { return data_while_busy_.exchange(false, std::memory_order_relaxed); }

    /**
     * 标记连接应在响应发送完毕后关闭；真正的 close 由 reactor 线程执行（消除 fd 重用竞争）
     */
    void request_close() { close_requested_.store(true, std::memory_order_relaxed); }
    /**
     * 查询连接是否已被请求关闭
     * \return true 表示应在当前响应发送完毕后关闭连接
     */
    bool close_requested() const { return close_requested_.load(std::memory_order_relaxed); }

    /**
     * 设置连接所属的子Reactor（连接"领养"时调用）
     * \param sr 所属子Reactor 指针
     */
    void set_owner(SubReactor* sr) { owner_ = sr; }
    /**
     * 获取连接所属的子Reactor
     * \return 所属子Reactor 指针；未领养时为空
     */
    SubReactor* owner() const { return owner_; }

    //解析请求
    HttpRequest req;
    //存储响应
    HttpResponse resp;

private:
    int fd_;
    SubReactor* owner_ = nullptr;
    std::atomic<bool> task_in_flight_{ false }; // 当前连接是否存在正在执行任务
    std::atomic<bool> data_while_busy_{ false }; // 当前连接存在数据（此时该连接被其他线程使用，子reactor通过该标志通知）
    std::atomic<bool> close_requested_{ false }; // 连接关闭
    mutable std::mutex buf_mu_;
    std::vector<char> read_buffer_;
    std::vector<char> write_buffer_;
};

} // namespace project

#endif // _CONNECTION_HPP
