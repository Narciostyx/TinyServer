#ifndef _HTTP_SESSION_HPP
#define _HTTP_SESSION_HPP

// HttpSession<Stream>：一条 TCP/TLS 连接对应一个会话对象，**由一条 C++20 协程驱动**。
//
// 设计说明：
//   * 传输层是 Boost.Asio，socket 的读写/超时/关闭全部由 io_context 驱动，
//     因此不需要 SubReactor 的 fd_contexts_ 表，也不需要 task_in_flight_ /
//     data_while_busy_ 原子来串行化连接——连接的身份就是本对象本身。
//   * 每个会话的 socket 建立在独立 strand 上，整条协程都在该 strand 上推进，
//     因此回调天然串行、**成员变量无需加锁**。
//   * 协程把"读头 → 100-continue → 读体 → 投递业务线程池 → 写响应 → keep-alive 循环"
//     写成了直线代码：原来的 do_x/on_x 回调状态机、writing_/reading_ 的边界判断、
//     "错误时先写响应再关闭"的嵌套闭包都消失了。
//   * **阻塞业务（Router/MySQL/Redis）仍走既有业务线程池**：通过
//     `async_initiate + use_awaitable` 定义自定义异步操作（见 dispatch()），
//     把任务投递到线程池并挂起协程，完成后在会话 strand 上恢复——
//     这样 io 线程永远不会被 SQL/Redis 阻塞（与旧回调版语义完全一致）。
//   * 定时器仍然作为"看门狗"独立运行（不 await）：到期即 hard_close()，
//     挂起的读写会以 operation_aborted 返回，协程据此退出。这样做的好处是
//     不需要依赖各版本 Boost 的 per-operation cancellation 支持。
//
// 关键不变量（务必保持）：
//   同一时刻对同一个 stream 只有一个挂起操作。协程天然满足这一点；
//   看门狗定时器只做"关闭 socket"，绝不发起新的读写。

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include "http.hpp"
#include "log.hpp"
#include "metrics.hpp"
#include "threadpool.hpp"

namespace project {

    using PlainStream = boost::asio::ip::tcp::socket;
    using TlsStream = boost::asio::ssl::stream<boost::asio::ip::tcp::socket>;

    template <class S> struct is_tls_stream : std::false_type {};
    template <> struct is_tls_stream<TlsStream> : std::true_type {};
    template <class S> inline constexpr bool is_tls_stream_v = is_tls_stream<S>::value;

    /**
     * 传输层共享状态。
     * 会话持有 shared_ptr 引用，因此即使 AsioTransport 先析构，计数器与关闭登记表依然有效
     * （避免"会话回调访问已销毁的 transport"这类悬垂）。
     */
    struct TransportState {
        std::atomic<std::uint64_t> active{ 0 };    // 当前活动会话数
        std::atomic<std::uint64_t> total{ 0 };     // 累计接受会话数
        std::atomic<std::uint64_t> next_id{ 0 };   // 会话 id 发号器（明文/TLS 共用）
        std::atomic<bool> stopping{ false };       // 是否已进入优雅停机
        std::atomic<bool> hard_stop{ false };      // 停机排空超时：立即硬关，不再等待响应

        // 由 AsioTransport 注入：活动会话数归零时触发（用于提前结束排空）
        std::function<void()> on_active_zero;

        std::mutex mu_;
        std::unordered_map<std::uint64_t, std::function<void()>> closers_;

        void add(std::uint64_t id, std::function<void()> closer) {
            std::lock_guard<std::mutex> lk(mu_);
            closers_.emplace(id, std::move(closer));
        }
        void remove(std::uint64_t id) {
            std::lock_guard<std::mutex> lk(mu_);
            closers_.erase(id);
        }

        /**
         * 通知所有存活会话收尾。
         * \param hard true 时置 hard_stop，会话无论处于读/写/等业务都会立刻硬关（排空超时用）
         */
        void notify_all(bool hard) {
            if (hard) hard_stop.store(true, std::memory_order_relaxed);
            std::unordered_map<std::uint64_t, std::function<void()>> local;
            {
                std::lock_guard<std::mutex> lk(mu_);
                local = closers_;   // 只拷贝闭包（内部是 weak_ptr），不破坏登记表以便二次通知
            }
            for (auto& kv : local)
                if (kv.second) kv.second();   // 闭包内部只做 post，不阻塞
        }
    };

    /** 会话运行参数（由 AsioTransport 按监听器填充，构造后只读） */
    struct SessionConfig {
        std::shared_ptr<boost::asio::ssl::context> tls_ctx;   // 为空 => 明文监听器
        bool is_tls = false;
        std::uint64_t body_limit = kDefaultBodyLimit;
        std::uint64_t header_limit = kDefaultHeaderLimit;
        int idle_timeout_seconds = 600;        // 等待"下一个请求头"的空闲上限
        int request_timeout_seconds = 30;      // 读请求体上限（Slowloris 防护）
        int handshake_timeout_seconds = 10;    // TLS 握手上限
        int shutdown_timeout_seconds = 5;      // TLS close_notify 等待上限 / 停机时的读宽限
        // 明文监听器仅用于跳转到 HTTPS（不读 body，响应后直接关闭）
        bool redirect_to_https = false;
        unsigned short https_port = 443;
        std::string fallback_host = "127.0.0.1";
        std::string peer = "-";                // 对端地址（用于日志）
        // 响应头相关（来自配置，避免把品牌名/追踪头名硬编码在协议逻辑里）
        std::string server_header = "TinyServer";
        std::string default_content_type = "application/json; charset=utf-8";
        std::string request_id_header = "X-Request-Id";
        // 快路径判定：命中时在 io 线程内直接处理，**不投递业务线程池**。
        // 用于 /healthz、/metrics 这类"只读原子量/拼字符串"的端点——否则业务线程池饱和时
        // 探针会拿到 503，被编排系统（k8s）误判为实例故障并重启。
        // 约定：被判定的端点内部不得有阻塞 IO。
        std::function<bool(const HttpRequest&)> inline_handler;
    };

    /**
     * 说明：这里原本想用一个自制的 awaiter（PoolTaskAwaitable）把阻塞任务丢进业务线程池。
     * 但 **Asio 的 awaitable promise 自带 await_transform**，会拦截协程体内的所有 co_await，
     * 只接受 Asio 自己的 awaitable / async operation，自制 awaiter 会被直接拒绝
     * （编译期报 "no matching function for call to awaitable_frame::await_transform"）。
     * 因此改用 Asio 官方的自定义异步操作写法：async_initiate + use_awaitable。
     * 具体实现见 HttpSession::dispatch()。
     */

    template <class Stream>
    class HttpSession : public std::enable_shared_from_this<HttpSession<Stream>> {
    public:
        using Handler = std::function<void(HttpRequest&, HttpResponse&)>;

        HttpSession(boost::asio::ip::tcp::socket&& sock,
                    SessionConfig cfg,
                    std::shared_ptr<TransportState> state,
                    ThreadPool& pool,
                    Handler handler)
            : cfg_(std::move(cfg)),
              state_(std::move(state)),
              pool_(pool),
              handler_(std::move(handler)),
              stream_(make_stream(std::move(sock), cfg_)),
              idle_timer_(stream_.get_executor()),
              guard_timer_(stream_.get_executor())
        {
        }

        /**
         * 析构：正常情况下会话都经过 close 流程收尾；这里是兜底——
         * 若会话被 io_context 析构连带销毁（未走 close），仍保证连接数指标与登记表一致。
         * 刻意不在此调用 on_active_zero：那会向正在析构的 io_context 投递任务。
         */
        ~HttpSession() {
            if (!closed_) {
                closed_ = true;
                state_->remove(id_);
                MetricsRegistry::instance().on_connection_closed();
                state_->active.fetch_sub(1, std::memory_order_acq_rel);
            }
        }

        /**
         * 启动会话：把整条处理流程作为一条协程挂到本会话的 strand 上。
         * 必须在对象已被 shared_ptr 拥有之后调用。
         */
        void start() {
            id_ = state_->next_id.fetch_add(1, std::memory_order_relaxed) + 1;
            state_->active.fetch_add(1, std::memory_order_relaxed);
            state_->total.fetch_add(1, std::memory_order_relaxed);
            MetricsRegistry::instance().on_connection_opened();

            // 登记"被 transport 强制关闭"的回调；用 weak_ptr 避免延长会话生命周期
            auto weak = this->weak_from_this();
            state_->add(id_, [weak]() {
                if (auto self = weak.lock())
                    boost::asio::post(self->stream_.get_executor(), [self]() { self->on_transport_stopping(); });
            });

            // 关键：把 shared_ptr 作为协程**参数**传进去。函数参数会在协程创建时
            // 立即拷贝进协程帧（早于 initial_suspend），所以从这一刻起会话的生命周期
            // 就绑在了协程帧上，不存在"start() 返回后对象被销毁、协程再执行"的窗口。
            auto self = this->shared_from_this();
            boost::asio::co_spawn(stream_.get_executor(), run(std::move(self)), boost::asio::detached);
        }

    private:
        // =====================================================================
        //  主协程：一条连接从握手到关闭的完整生命周期
        // =====================================================================
        /**
         * \param self_anchor 仅为把会话生命周期绑定到协程帧上而传入（见 start()）
         */
        boost::asio::awaitable<void> run(std::shared_ptr<HttpSession> self_anchor)
        {
            (void)self_anchor;   // 生命周期锚点，不参与逻辑
            try
            {
                boost::beast::error_code ec;

                // ---------------- TLS 握手（仅 TLS 会话） ----------------
                if constexpr (is_tls_stream_v<Stream>) {
                    arm_handshake_timeout();
                    co_await stream_.async_handshake(
                        boost::asio::ssl::stream_base::server,
                        boost::asio::redirect_error(boost::asio::use_awaitable, ec));
                    guard_timer_.cancel();
                    if (closed_) co_return;
                    if (ec) {
                        MetricsRegistry::instance().on_tls_handshake(false);
                        LOG_WARN("TLS handshake failed from " + cfg_.peer + ": " + ec.message());
                        co_await close_gracefully();
                        co_return;
                    }
                    MetricsRegistry::instance().on_tls_handshake(true);
                }

                // ---------------- keep-alive 请求循环 ----------------
                for (;;)
                {
                    if (closed_) break;
                    // 停机中且本连接空闲（等待下一个请求）：直接关闭，符合 RFC 7230 §6.5
                    if (state_->stopping.load(std::memory_order_relaxed)) break;

                    // ---- 1) 读请求头 ----
                    auto parser = std::make_unique<HttpRequestParser>();
                    parser->body_limit(cfg_.body_limit);
                    parser->header_limit(static_cast<std::uint32_t>(
                        cfg_.header_limit > 0xFFFFFFFFull ? 0xFFFFFFFFull : cfg_.header_limit));

                    reading_ = true;
                    arm_idle(cfg_.idle_timeout_seconds);
                    co_await boost::beast::http::async_read_header(
                        stream_, buffer_, *parser,
                        boost::asio::redirect_error(boost::asio::use_awaitable, ec));
                    idle_timer_.cancel();
                    reading_ = false;
                    if (closed_) break;

                    if (ec) {
                        if (ec == boost::beast::http::error::end_of_stream || ec == boost::asio::error::eof)
                            break;                                    // 对端正常关闭
                        if (ec == boost::asio::error::operation_aborted)
                            break;                                    // 看门狗/强制关闭导致
                        if (ec == boost::beast::http::error::header_limit) {
                            co_await write_error(boost::beast::http::status::request_header_fields_too_large,
                                                 "Request header too large", std::string());
                            break;
                        }
                        // 明文端口收到非 HTTP 字节（最常见的就是客户端误用 https:// 访问明文端口）
                        if constexpr (!is_tls_stream_v<Stream>) {
                            LOG_WARN("会话 " + std::to_string(id_) + " 从 " + cfg_.peer
                                     + " 收到的数据不是合法 HTTP（" + ec.message()
                                     + "）。若客户端用的是 https:// 访问本明文端口，请改用 HTTPS 端口 Https_port="
                                     + std::to_string(cfg_.https_port) + "。");
                        }
                        LOG_DEBUG("session " + std::to_string(id_) + " read header failed: " + ec.message());
                        break;                                        // 头都没解析出来，无法生成有意义的响应
                    }

                    const HttpRequest& req = parser->get();
                    const unsigned version = req.version();
                    const bool is_head = (req.method() == boost::beast::http::verb::head);
                    const std::string request_id = request_id_for(req);

                    // ---- 2) 明文监听器：只负责把流量引导到 HTTPS ----
                    if (cfg_.redirect_to_https) {
                        co_await write_redirect(req, request_id);
                        break;
                    }

                    // ---- 3) 长度预检：必然超限时直接 413，绝不先回 100 Continue ----
                    if (const auto remaining = parser->content_length_remaining()) {
                        if (*remaining > cfg_.body_limit) {
                            co_await write_error(boost::beast::http::status::payload_too_large,
                                                 "Request body too large", request_id);
                            break;
                        }
                    }

                    // ---- 4) Expect: 100-continue（客户端在收到 100 之前不会发 body） ----
                    const auto expect = req[boost::beast::http::field::expect];
                    if (!expect.empty() && boost::beast::iequals(expect, "100-continue") && !parser->is_done()) {
                        static const std::string kContinue = "HTTP/1.1 100 Continue\r\n\r\n";
                        writing_ = true;
                        co_await boost::asio::async_write(
                            stream_, boost::asio::buffer(kContinue),
                            boost::asio::redirect_error(boost::asio::use_awaitable, ec));
                        writing_ = false;
                        if (closed_) break;
                        if (ec) break;
                    }

                    // ---- 5) 读请求体 ----
                    reading_ = true;
                    arm_idle(cfg_.request_timeout_seconds);
                    co_await boost::beast::http::async_read(
                        stream_, buffer_, *parser,
                        boost::asio::redirect_error(boost::asio::use_awaitable, ec));
                    idle_timer_.cancel();
                    reading_ = false;
                    if (closed_) break;

                    if (ec) {
                        if (ec == boost::beast::http::error::body_limit) {
                            co_await write_error(boost::beast::http::status::payload_too_large,
                                                 "Request body too large", request_id);
                            break;
                        }
                        if (ec == boost::beast::http::error::end_of_stream || ec == boost::asio::error::eof)
                            break;
                        if (ec == boost::asio::error::operation_aborted)
                            break;
                        co_await write_error(boost::beast::http::status::bad_request, "Bad Request", request_id);
                        break;
                    }

                    // keep_alive 取自 parser：正确处理 HTTP/1.0 默认关闭、1.1 默认复用、Connection 头
                    const bool req_keep_alive = parser->keep_alive();
                    auto req_msg = std::make_shared<HttpRequest>(parser->release());
                    parser.reset();

                    // ---- 6) 业务处理：快路径直接在本协程内做，其余投递业务线程池 ----
                    auto resp = std::make_shared<HttpResponse>();

                    if (cfg_.inline_handler && cfg_.inline_handler(*req_msg)) {
                        // 快路径：不经业务线程池，避免"池饱和 => 探针 503"
                        try {
                            handler_(*req_msg, *resp);
                        }
                        catch (const std::exception& e) {
                            set_json_error(*resp, boost::beast::http::status::internal_server_error,
                                           "Internal Server Error");
                            LOG_ERR(std::string("Inline handler exception: ") + e.what());
                        }
                        catch (...) {
                            set_json_error(*resp, boost::beast::http::status::internal_server_error,
                                           "Internal Server Error");
                            LOG_ERR("Inline handler unknown exception.");
                        }
                        const bool keep_inline = co_await write_response(std::move(resp), version,
                                                                        req_keep_alive, request_id, is_head);
                        if (!keep_inline || closed_) break;
                        continue;
                    }

                    const bool queued = co_await dispatch(req_msg, resp);
                    if (closed_) break;

                    if (!queued) {
                        // 线程池过载：明确回 503 + Retry-After（旧实现是直接断开，客户端无从判断）
                        LOG_WARN("Thread pool overloaded, reply 503 to " + cfg_.peer);
                        set_json_error(*resp, boost::beast::http::status::service_unavailable,
                                       "Server busy, please retry later");
                        resp->set(boost::beast::http::field::retry_after, "1");
                        co_await write_response(std::move(resp), version, false, request_id, is_head);
                        break;
                    }

                    // ---- 7) 写响应，并决定是否继续复用连接 ----
                    const bool keep = co_await write_response(std::move(resp), version,
                                                              req_keep_alive, request_id, is_head);
                    if (!keep || closed_) break;
                }

                co_await close_gracefully();
            }
            catch (const std::exception& e) {
                LOG_ERR(std::string("HttpSession 协程异常: ") + e.what());
                hard_close();
            }
            catch (...) {
                LOG_ERR("HttpSession 协程未知异常。");
                hard_close();
            }
        }

        // =====================================================================
        //  子协程
        // =====================================================================

        /**
         * 把业务处理投递到线程池并等待完成。
         *
         * 实现方式：**用一个 steady_timer 当"任务完成信号"**。
         *   * 业务任务（阻塞的 Router/MySQL/Redis）投进 ThreadPool 后，协程 co_await 该定时器的
         *     async_wait（定时器不 arm，等于无限期等待），任务跑完时在会话 strand 上 cancel() 唤醒。
         *   * 之所以不用自制 awaiter：Asio 的 awaitable promise 自带 await_transform，
         *     会拦截协程体内所有 co_await，只接受 Asio 自己的 awaitable / async operation
         *     （自制 awaiter 会报 "no matching await_transform"）。
         *   * 之所以也不用 async_initiate：显式写 async_initiate<use_awaitable_t<>, Sig> 时，
         *     const 的 use_awaitable 对象无法绑定到它要求的非 const 左值引用上，
         *     且显式模板实参会被匹配进 Signatures 包而推导失败。
         *     而 async_wait + redirect_error(use_awaitable, ec) 这种写法本文件已在多处使用、可编译，
         *     所以这里复用它——依赖最少、最稳。
         *   * 池满时**不等待**，直接返回 false，调用方回 503（背压语义与旧回调版一致）。
         *
         * \return true 表示已入队并执行完毕；false 表示线程池已满
         */
        boost::asio::awaitable<bool> dispatch(std::shared_ptr<HttpRequest> req,
                                              std::shared_ptr<HttpResponse> resp)
        {
            // 保活：线程池任务只持有裸 this，靠协程帧里的 shared_ptr 保证对象存活
            auto self = this->shared_from_this();
            const auto ex = stream_.get_executor();
            // 用 shared_ptr 持有信号定时器：任务在池线程上完成时仍要 cancel 它，
            // 若用栈上对象，协程帧被销毁后就是悬垂引用。
            auto signal = std::make_shared<boost::asio::steady_timer>(ex);

            processing_ = true;
            const bool queued = pool_.try_enqueue([this, req, resp, ex, signal, self]() {
                try {
                    handler_(*req, *resp);
                }
                catch (const std::exception& e) {
                    set_json_error(*resp, boost::beast::http::status::internal_server_error,
                                   "Internal Server Error");
                    LOG_ERR(std::string("Business handler exception: ") + e.what());
                }
                catch (...) {
                    set_json_error(*resp, boost::beast::http::status::internal_server_error,
                                   "Internal Server Error");
                    LOG_ERR("Business handler unknown exception.");
                }
                // 回到会话 strand 唤醒协程（timer 的成员函数不在跨线程并发调用）
                // 注意：本版本 Boost 已移除 cancel(error_code&) 重载，只能用无参 cancel()
                boost::asio::post(ex, [signal]() { (void)signal->cancel(); });
            });

            if (!queued) {
                // 线程池过载：不挂起，直接告诉调用方回 503
                processing_ = false;
                co_return false;
            }

            boost::system::error_code ec;
            co_await signal->async_wait(
                boost::asio::redirect_error(boost::asio::use_awaitable, ec));
            processing_ = false;
            co_return true;
        }

        /**
         * 写响应。
         * \param req_keep_alive 请求是否允许复用连接
         * \param is_head 请求方法是否为 HEAD（必须发头不发 body）
         * \return true 表示可以继续 keep-alive 循环
         */
        boost::asio::awaitable<bool> write_response(std::shared_ptr<HttpResponse> resp,
                                                    unsigned version,
                                                    bool req_keep_alive,
                                                    std::string request_id,
                                                    bool is_head)
        {
            const bool keep = req_keep_alive && !close_after_write_
                && !state_->stopping.load(std::memory_order_relaxed);

            resp->version(version == 0 ? 11 : version);
            apply_default_response_headers(*resp, cfg_.server_header, cfg_.default_content_type);
            if (!cfg_.request_id_header.empty() && !request_id.empty())
                resp->set(cfg_.request_id_header, request_id);
            resp->prepare_payload();                                   // 依据 body 设定 Content-Length
            resp->set(boost::beast::http::field::connection, keep ? "keep-alive" : "close");

            // HEAD：必须发送与 GET 相同的头（含 Content-Length），但**不发送 body**。
            // Beast 的 serializer 没有 skip 开关（basic_parser 上的 skip(bool) 是解析侧的），
            // 因此这里直接把 body 清空——prepare_payload() 上面已经把 Content-Length 写成
            // GET 应有的长度，清空 body 不会改动该头，于是"头保留、body 不发"。
            // 不这么做的话，HEAD 响应会带上 body，污染后续 keep-alive 流。
            if (is_head)
                resp->body().clear();

            boost::beast::error_code ec;
            writing_ = true;
            co_await boost::beast::http::async_write(
                stream_, *resp, boost::asio::redirect_error(boost::asio::use_awaitable, ec));
            writing_ = false;

            if (ec) {
                LOG_DEBUG("session " + std::to_string(id_) + " write failed: " + ec.message());
                co_return false;
            }
            co_return keep;
        }

        /// 错误响应：写完即关闭（无法确认已读到消息边界，必须关闭以重新同步）
        /// \note request_id 按值传递：本函数是协程，引用参数会跨挂起点，
        ///       而调用处可能传临时量；按值传递彻底规避生命周期问题。
        boost::asio::awaitable<void> write_error(boost::beast::http::status status,
                                                 const std::string& msg,
                                                 std::string request_id)
        {
            auto resp = std::make_shared<HttpResponse>();
            set_json_error(*resp, status, msg);
            close_after_write_ = true;
            co_await write_response(std::move(resp), 11, false, std::move(request_id), false);
        }

        /// 明文监听器的 301 跳转（只读了请求头，响应后直接关闭）
        boost::asio::awaitable<void> write_redirect(const HttpRequest& req, const std::string& request_id)
        {
            std::string host(req[boost::beast::http::field::host]);
            if (host.empty()) {
                host = cfg_.fallback_host;
            } else if (const auto colon = host.find(':'); colon != std::string::npos) {
                host.erase(colon);      // 去掉客户端带来的端口（通常是 80）
            }

            std::string location = "https://" + host;
            if (cfg_.https_port != 443)
                location += ":" + std::to_string(cfg_.https_port);

            std::string target(req.target());
            if (target.empty() || target.front() != '/')
                target = "/" + target;
            location += target;

            auto resp = std::make_shared<HttpResponse>();
            set_json_error(*resp, boost::beast::http::status::moved_permanently, "Please use HTTPS");
            resp->set(boost::beast::http::field::location, location);

            close_after_write_ = true;
            co_await write_response(std::move(resp), req.version(), false, request_id, false);
        }

        /// 优雅关闭：TLS 会话发送 close_notify（协程模型下此刻必然没有挂起的读写）
        boost::asio::awaitable<void> close_gracefully()
        {
            if (!begin_close()) co_return;
            if constexpr (is_tls_stream_v<Stream>) {
                arm_shutdown_timeout();     // 对端迟迟不回应 close_notify 时兜底强关
                boost::beast::error_code ec;
                co_await stream_.async_shutdown(
                    boost::asio::redirect_error(boost::asio::use_awaitable, ec));
                guard_timer_.cancel();
            }
            boost::system::error_code ignored;
            boost::beast::get_lowest_layer(stream_).close(ignored);
        }

        // =====================================================================
        //  关闭与账务
        // =====================================================================

        /// 幂等账务：取消定时器 / 注销登记 / 更新指标。返回 true 表示本次调用完成了收尾
        bool begin_close()
        {
            if (closed_) return false;
            closed_ = true;
            idle_timer_.cancel();
            guard_timer_.cancel();
            state_->remove(id_);
            MetricsRegistry::instance().on_connection_closed();
            if (state_->active.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                if (state_->on_active_zero)
                    state_->on_active_zero();
            }
            return true;
        }

        /// 硬关闭：直接关 socket，让挂起的读写以 operation_aborted 返回
        void hard_close()
        {
            if (!begin_close()) return;
            boost::system::error_code ignored;
            boost::beast::get_lowest_layer(stream_).close(ignored);
        }

        /// transport 进入停机：空闲连接立即关闭；正在处理/写响应的等写完再关
        void on_transport_stopping()
        {
            if (closed_) return;

            if (state_->hard_stop.load(std::memory_order_relaxed)) {
                hard_close();
                return;
            }
            if (reading_) {
                close_after_write_ = true;
                // 缩短读等待：给排空一个明确的宽限期，避免 keep-alive 连接把停机拖到 idle_timeout
                arm_idle(cfg_.shutdown_timeout_seconds);
                return;
            }
            if (writing_ || processing_) {
                close_after_write_ = true;
                return;
            }
            hard_close();   // 空闲（正挂在读请求头上或尚未开始）=> 直接关
        }

        // =====================================================================
        //  看门狗定时器（不 await，到期即关闭 socket）
        // =====================================================================

        void arm_idle(int seconds)
        {
            // 注意：Boost.Asio 1.87 起弃用、随后移除了 expires_after/cancel 的
            // error_code 重载，因此这里只用单参形式（duration 恒为正，不会抛异常）。
            idle_timer_.expires_after(std::chrono::seconds(seconds > 0 ? seconds : 1));
            auto self = this->shared_from_this();
            idle_timer_.async_wait([self](const boost::system::error_code& e) {
                if (e) return;              // 被取消 => 流程正常推进
                LOG_WARN("session " + std::to_string(self->id_) + " (" + self->cfg_.peer
                         + ") 空闲/读超时，关闭连接。");
                self->hard_close();
            });
        }

        void arm_handshake_timeout()
        {
            guard_timer_.expires_after(std::chrono::seconds(cfg_.handshake_timeout_seconds > 0
                ? cfg_.handshake_timeout_seconds : 1));
            auto self = this->shared_from_this();
            guard_timer_.async_wait([self](const boost::system::error_code& e) {
                if (e || self->closed_) return;
                MetricsRegistry::instance().on_tls_handshake(false);
                LOG_WARN("TLS handshake timeout from " + self->cfg_.peer);
                self->hard_close();
            });
        }

        void arm_shutdown_timeout()
        {
            guard_timer_.expires_after(std::chrono::seconds(cfg_.shutdown_timeout_seconds > 0
                ? cfg_.shutdown_timeout_seconds : 1));
            auto self = this->shared_from_this();
            guard_timer_.async_wait([self](const boost::system::error_code& e) {
                if (e) return;
                boost::system::error_code ignored;
                boost::beast::get_lowest_layer(self->stream_).close(ignored);
            });
        }

        // =====================================================================
        //  工具
        // =====================================================================

        static Stream make_stream(boost::asio::ip::tcp::socket&& sock, const SessionConfig& cfg) {
            if constexpr (is_tls_stream_v<Stream>) {
                return Stream(std::move(sock), *cfg.tls_ctx);
            } else {
                (void)cfg;
                return Stream(std::move(sock));
            }
        }

        /// 回显客户端带来的追踪头，没有则生成一个
        std::string request_id_for(const HttpRequest& req) const
        {
            if (!cfg_.request_id_header.empty()) {
                const auto incoming = req[cfg_.request_id_header];
                if (!incoming.empty())
                    return std::string(incoming);
            }
            return make_request_id();
        }

        static std::string make_request_id() {
            static std::atomic<std::uint64_t> counter{ 0 };
            const auto n = counter.fetch_add(1, std::memory_order_relaxed);
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            char buf[48] = {};
            std::snprintf(buf, sizeof(buf), "%llx-%llx",
                          static_cast<unsigned long long>(us), static_cast<unsigned long long>(n));
            return std::string(buf);
        }

        SessionConfig cfg_;
        std::shared_ptr<TransportState> state_;
        ThreadPool& pool_;
        Handler handler_;

        Stream stream_;
        boost::beast::flat_buffer buffer_;

        boost::asio::steady_timer idle_timer_;
        boost::asio::steady_timer guard_timer_;

        std::uint64_t id_ = 0;

        // 仅在"跨线程/跨协程观察"时需要保留的状态；请求级变量都是 run() 内的局部量。
        // 这三个标志只在本会话的 strand 上读写，无需原子。
        bool reading_ = false;              // 正挂在读操作上
        bool writing_ = false;             // 正挂在写操作上
        bool processing_ = false;          // 已投递业务线程池、尚未拿到响应
        bool close_after_write_ = false;   // 停机/错误收尾：写完当前响应即关闭
        bool closed_ = false;              // 已关闭（幂等标记）
    };

} // namespace project

#endif // _HTTP_SESSION_HPP
