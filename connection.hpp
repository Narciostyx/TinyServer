#ifndef _CONNECTION_HPP
#define _CONNECTION_HPP

#include <string>
#include <vector>
#include <memory>
#include <chrono>
#include "http.hpp"

namespace project {

//每个文件描述符对应一个读缓冲区与写缓冲区
class Connection : public std::enable_shared_from_this<Connection> {
public:
    Connection(int fd) : fd_(fd) {
        read_buffer_.reserve(8192);
        write_buffer_.reserve(8192);
        update_last_active_time();
    }

    ~Connection() = default;

    //返回对应的fd
    int get_fd() const { return fd_; }


    //增添读缓冲区数据
    void append_read_data(const char* data, size_t len) {
        read_buffer_.insert(read_buffer_.end(), data, data + len);
    }


    //以字符串形式返回底层读缓冲区内容，然后清空读缓冲区
    std::string take_read_buffer() {
        std::string res(read_buffer_.begin(), read_buffer_.end());
        read_buffer_.clear();
        return res;
    }


    //增添写缓冲区数据
    void append_write_data(const std::string& data) {
        write_buffer_.insert(write_buffer_.end(), data.begin(), data.end());
    }
    //返回读缓冲区
    const std::vector<char>& write_buffer() const { return write_buffer_; }
    //清空读缓冲区
    void clear_write_buffer() { write_buffer_.clear(); }

    //解析请求
    HttpRequest req;
    //存储响应
    HttpResponse resp;

    void update_last_active_time() { last_active_time_ = std::chrono::steady_clock::now(); }
    std::chrono::steady_clock::time_point last_active_time() const { return last_active_time_; }

private:
    int fd_;
    std::vector<char> read_buffer_;
    std::vector<char> write_buffer_;
    std::chrono::steady_clock::time_point last_active_time_;
};

} // namespace project

#endif // _CONNECTION_HPP
