#ifndef _ACCEPTOR_HPP
#define _ACCEPTOR_HPP

#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>

#include "log.hpp"

namespace project {

    // 封装accept函数
    class Acceptor {
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