#include "acceptor.hpp"

// 构造函数：创建监听socket并绑定端口
project::Acceptor::Acceptor(int port) {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    int opt = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (::bind(listen_fd_, (sockaddr*)&addr, sizeof(addr)))
    {
        LOG_ERR("Bind to the specific port " + std::to_string(port) + " failed:" + strerror(errno));
        exit(exit_code = 3);
    }
    if (::listen(listen_fd_, SOMAXCONN))
    {
        LOG_ERR("Listen on the specific port " + std::to_string(port) + " failed:" + strerror(errno));
        exit(exit_code = 3);
    }
}

// 析构函数：关闭监听socket
project::Acceptor::~Acceptor() {
    if (listen_fd_ >= 0) close(listen_fd_);
}

// 获取监听socket fd
int project::Acceptor::get_fd() const {
    return listen_fd_;
}

// 接受新连接
int project::Acceptor::accept() {
    sockaddr_in client;
    socklen_t client_len = sizeof(client);
    char ip[INET_ADDRSTRLEN];

    int fd = ::accept(listen_fd_, (sockaddr*)&client, &client_len);
    if (fd < 0) {
        return -1;
    }

    ::inet_ntop(AF_INET, &client.sin_addr, ip, sizeof(ip));

    // IP限制逻辑：目前仅允许本地回环地址 127.0.0.1 访问
    std::string client_ip(ip);
    if (client_ip != "127.0.0.1") {
        LOG_WARN("Blocked connection from IP " + client_ip + ": only 127.0.0.1 is allowed.");
        ::close(fd);
        return -1;
    }

    LOG_INFO(std::format("Connect to the host {}:{} with fd {}.", ip, ::ntohs(client.sin_port), fd));
    return fd;
}