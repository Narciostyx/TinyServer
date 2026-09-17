#include "server.hpp"

#include <csignal>

// 入口说明：
//   1) 忽略 SIGPIPE。对端提前关闭时，写 socket / TLS 记录会触发 SIGPIPE，
//      默认行为是终止进程；忽略后写操作以 EPIPE 的形式由 Asio 作为 error_code 上报。
//   2) SIGINT/SIGTERM/SIGHUP 不再用 std::signal 注册回调，
//      而是由 AsioTransport 内部的 boost::asio::signal_set 处理：
//      旧实现在信号处理器里调用 Server::stop()（会加锁、分配内存、打日志），
//      那属于 async-signal-unsafe；signal_set 把信号投递到 io_context 线程，天然安全。
//      SIGINT/SIGTERM -> 优雅停机（第二次再按则强制退出）；SIGHUP -> 热重载 TLS 证书。
int main(int argc, char* argv[])
{
    std::signal(SIGPIPE, SIG_IGN);

    project::Server server(argc, argv);

    if (!server.init())
        return -1;

    server.start(); // 阻塞运行 io_context，直到收到停机信号并排空完成
    return 0;
}
