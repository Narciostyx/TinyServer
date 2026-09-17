#ifndef _ASIO_TRANSPORT_HPP
#define _ASIO_TRANSPORT_HPP

// AsioTransport：基于 Boost.Asio 的 HTTP/HTTPS 传输层，替代原 epoll 主从 Reactor。
//
// 结构：
//   io_context（默认 N 个线程）
//     ├─ http  tcp::acceptor            —— 明文监听（可关闭；可配置为仅 301 跳转 HTTPS）
//     ├─ https tcp::acceptor + SSL_CTX  —— TLS 监听（TLS1.2+，ALPN，会话复用，可选 mTLS/SNI）
//     ├─ signal_set                     —— SIGINT/SIGTERM 优雅停机，SIGHUP 热重载证书
//     └─ steady_timer                   —— 停机排空超时
//   每条连接 = 一个 HttpSession<Stream>，socket 建于独立 strand ⇒ 回调串行、无需加锁。
//
// 与业务的边界：Handler（Server::handle_request）在**业务线程池**内执行，因为
// Router/DataService 内部是阻塞的 MySQL/Redis 调用；io 线程绝不执行阻塞操作。

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/steady_timer.hpp>

#include "http.hpp"
#include "http_session.hpp"
#include "threadpool.hpp"

namespace project {

    /** 单个监听器配置 */
    struct ListenerOptions {
        bool enable = true;
        std::string address = "127.0.0.1";
        unsigned short port = 8080;
    };

    /** TLS 配置 */
    struct TlsOptions {
        bool enable = false;
        std::string cert_file;
        std::string key_file;
        std::string ca_file;                 // 非空 => 启用客户端证书校验（mTLS）
        bool require_client_cert = false;    // true => 必须提供客户端证书
        int verify_depth = 4;                // 客户端证书链校验深度
        std::string min_version = "1.2";     // 1.0 / 1.1 / 1.2 / 1.3
        std::string ciphers;                 // TLS1.2 及以下套件（OpenSSL 语法）
        std::string ciphersuites;            // TLS1.3 套件
        bool session_tickets = true;
        int session_ticket_count = 2;
        std::string session_id_context = "TinyServer";
        std::string alpn = "http/1.1";       // 逗号分隔的 ALPN 协议名（上 HTTP/2 时追加 h2）
        // 证书/私钥缺失时自动生成自签证书。HTTPS 默认开启，若必须先手工生成证书才能启动，
        // 默认配置就永远起不来；生产请置 0 并配置受信任证书。
        bool auto_self_signed = true;
        // SNI 多证书：host -> (cert, key)。空则只用上面的默认证书。
        std::vector<std::pair<std::string, std::pair<std::string, std::string>>> sni_certs;
    };

    /** 传输层总配置（由 Server 从 Config 映射而来） */
    struct TransportOptions {
        ListenerOptions http;
        ListenerOptions https;
        TlsOptions tls;
        std::string allow_peers = "127.0.0.1/32,::1/128";  // 逗号分隔 IP/CIDR；空 = 不限制
        std::size_t max_connections = 5000;
        std::uint64_t body_limit = kDefaultBodyLimit;
        std::uint64_t header_limit = kDefaultHeaderLimit;
        int idle_timeout_seconds = 600;         // keep-alive 空闲（等待下一个请求头）
        int request_timeout_seconds = 30;       // 读请求体上限
        int handshake_timeout_seconds = 10;     // TLS 握手上限
        int shutdown_timeout_seconds = 10;      // 停机排空超时
        int shutdown_drain_grace_ms = 500;      // 硬停机后等待会话关闭回调跑完的宽限
        int io_threads = 0;                     // 0 => max(2, hardware_concurrency)
        bool redirect_http_to_https = false;    // 明文监听器只做 301
        // 监听器行为
        int listen_backlog = 0;                 // 0 => SOMAXCONN
        bool listen_dual_stack = true;          // IPv6 监听地址是否同时接受 IPv4
        bool tcp_keepalive = true;              // 已接受连接是否开启 TCP keepalive
        // HTTP 响应头（透传给每个会话，避免把品牌名/追踪头名硬编码）
        std::string server_header = "TinyServer";
        std::string default_content_type = "application/json; charset=utf-8";
        std::string request_id_header = "X-Request-Id";
        // 快路径判定（透传给每个会话）：命中时在 io 线程内直接处理，不投业务线程池
        std::function<bool(const HttpRequest&)> inline_handler;
    };

    class AsioTransport {
    public:
        /// 业务处理回调（在业务线程池内执行，必须线程安全）
        using Handler = std::function<void(HttpRequest&, HttpResponse&)>;

        AsioTransport(TransportOptions opt, ThreadPool& pool, Handler handler);
        ~AsioTransport();

        AsioTransport(const AsioTransport&) = delete;
        AsioTransport& operator=(const AsioTransport&) = delete;

        /**
         * 建立 TLS 上下文并绑定所有监听端口。
         * \throw Err(kErrType::Tls_init)      TLS 配置/证书加载失败
         * \throw Err(kErrType::Listen_init)   bind/listen 失败
         * \throw Err(kErrType::Reactor_init)  监听 socket 创建失败
         * \throw Err(kErrType::defaultType)   配置不合法（地址非法、CIDR 语法错误、未启用任何监听等）
         */
        void init();

        /// io 线程数（含调用 run() 的那个线程）
        int thread_count() const;

        /// 启动 thread_count()-1 个额外 io 线程
        void start_threads();

        /// 在当前线程运行 io_context（阻塞至停机）
        void run();

        /// join 所有额外 io 线程
        void join_threads();

        /// 请求优雅停机（幂等、线程安全；可在任意线程/信号回调中调用）
        void stop();

        /// 热重载证书（SIGHUP）：新建连接使用新证书，已建立连接不受影响
        bool reload_tls();

        bool tls_enabled() const { return opt_.tls.enable; }

    private:
        struct Listener {
            ListenerOptions opt;
            bool is_tls = false;
            boost::asio::ip::tcp::acceptor acceptor;

            explicit Listener(boost::asio::io_context& ioc) : acceptor(ioc) {}
        };

        // CIDR/IP 白名单规则（简化版 ACL；空列表 = 不限制）
        struct CidrRule {
            bool allow_all = false;
            bool is_v4 = true;
            std::array<std::uint8_t, 16> network{};
            int bits = 0;

            bool matches(const boost::asio::ip::address& addr) const;
        };

        void build_options_validated();
        void open_listener(Listener& l);
        void do_accept(const std::shared_ptr<Listener>& l);
        void on_accept(const std::shared_ptr<Listener>& l,
                       const boost::system::error_code& ec,
                       boost::asio::ip::tcp::socket sock);

        void arm_signals();
        void on_signal(const boost::system::error_code& ec, int signum);
        void begin_shutdown();
        void finish_shutdown();
        void stop_event_loop();

        void rebuild_tls();
        std::shared_ptr<boost::asio::ssl::context> make_tls_context(const std::string& cert,
                                                                   const std::string& key,
                                                                   bool install_sni_cb) const;
        std::shared_ptr<boost::asio::ssl::context> tls_ctx_snapshot() const;
        SSL_CTX* find_sni_ctx(const std::string& host) const;

        static int alpn_select_cb(SSL* ssl, const unsigned char** out, unsigned char* outlen,
                                  const unsigned char* in, unsigned int inlen, void* arg);
        static int servername_cb(SSL* ssl, int* al, void* arg);

        bool peer_allowed(const boost::asio::ip::address& addr) const;

        TransportOptions opt_;
        ThreadPool& pool_;
        Handler handler_;

        boost::asio::io_context ioc_;
        std::shared_ptr<TransportState> state_;
        std::vector<std::shared_ptr<Listener>> listeners_;

        // 默认 TLS 上下文与 SNI 上下文；历史上下文只退役不释放，
        // 因为已建立的连接/正在握手的连接可能仍引用旧 SSL_CTX（SIGHUP 热重载场景）
        mutable std::mutex tls_mu_;
        std::shared_ptr<boost::asio::ssl::context> tls_ctx_;
        std::unordered_map<std::string, std::shared_ptr<boost::asio::ssl::context>> sni_ctxs_;
        std::vector<std::shared_ptr<boost::asio::ssl::context>> retired_ctxs_;
        // ALPN 线格式（长度前缀拼接）。init() 中构建一次后只读，
        // 因此可以把它的地址交给 SSL_CTX 的回调参数（生命周期与 transport 一致）。
        std::string alpn_wire_;

        std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work_;
        boost::asio::steady_timer drain_timer_;
        boost::asio::signal_set signals_;

        std::vector<std::thread> threads_;
        std::vector<CidrRule> acl_;

        std::atomic<bool> stopping_{ false };
        std::atomic<bool> finished_{ false };

        // 与 state_->on_active_zero 互斥：析构函数先置 alive_=false，
        // 保证"会话在 transport 析构后回调"不会访问已销毁的 ioc_
        mutable std::mutex life_mu_;
        bool alive_ = true;
    };

} // namespace project

#endif // _ASIO_TRANSPORT_HPP
