#ifndef _ACCEPTOR_HPP
#define _ACCEPTOR_HPP

// =============================================================================
// 已废弃：裸 accept 封装（含硬编码 127.0.0.1 白名单），
// 已被 AsioTransport 的 ip::tcp::acceptor + open_listener()/do_accept() 取代
// （监听地址与来源白名单改为 Listen_address / Allow_peers 配置，支持 CIDR）。
// 保留仅供对照阅读，**不参与编译**。
// =============================================================================

#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>

#include "log.hpp"

namespace project {

    /**
     * 封装accept函数
     *
     * \deprecated 请改用 ../asio_transport.hpp 的监听器（AsioTransport::open_listener/do_accept）。
     */
    class [[deprecated("Acceptor 已被 AsioTransport 的 ip::tcp::acceptor 取代，请使用 asio_transport.hpp")]] Acceptor {
    public:
        Acceptor(unsigned short int port);
        ~Acceptor();

        int get_fd() const; // 获取绑定的监听套接字
        /**
         * 获取请求连接套接字
         * 
         * \return 可能为-1，需要对其进行判断
         */
        int accept();

    private:
        int listen_fd_ = -1; // 监听socket
    };

}
#endif