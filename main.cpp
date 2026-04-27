#include "server.hpp"

#include <atomic>
#include <csignal>

namespace
{
    std::atomic<bool> g_stop_requested{ false };
    project::Server* g_server = nullptr;

    void on_terminate_signal(int)
    {
        g_stop_requested.store(true, std::memory_order_relaxed);
        if (g_server)
            g_server->stop(); // 仅发出停止请求，不在信号里delete/exit
    }
}

int main(int argc, char* argv[])
{
    std::signal(SIGINT, on_terminate_signal);
    std::signal(SIGTERM, on_terminate_signal);

    project::Server server(argc, argv);
    g_server = &server;

    if (!server.init())
        return 1;

    server.start(); // 后续需要让start能因stop而返回，才能“正常退出”
    return 0;
}
