#ifndef _ACCEPTOR_HPP
#define _ACCEPTOR_HPP

#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>

#include "log.hpp"

namespace project {

    // Acceptor类：负责监听端口并接受新连接
    class Acceptor {
    public:
        Acceptor(unsigned short int port);
        ~Acceptor();

        int get_fd() const;      // 获取监听socket fd
        int accept();            // 接受新连接

    private:
        int listen_fd_ = -1;        // 监听socket
    };

}
#endif