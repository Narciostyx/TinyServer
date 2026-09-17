#include "server.hpp"

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sys/syscall.h>
#include <unistd.h>

#include "error.hpp"
#include "metrics.hpp"
#include "redis_store.hpp"

namespace project
{
	namespace
	{
		/**
		 * 按逗号切分并去掉首尾空白（用于解析配置里的逗号分隔列表）
		 * \param spec 形如 "a,b,c" 的原始串
		 * \return 切分后的非空项
		 */
		std::vector<std::string> split_csv(const std::string& spec)
		{
			std::vector<std::string> out;
			std::size_t pos = 0;
			while (pos <= spec.size())
			{
				const std::size_t comma = spec.find(',', pos);
				std::string item = spec.substr(
					pos, comma == std::string::npos ? std::string::npos : comma - pos);
				pos = (comma == std::string::npos) ? spec.size() + 1 : comma + 1;

				while (!item.empty() && std::isspace(static_cast<unsigned char>(item.front())))
					item.erase(item.begin());
				while (!item.empty() && std::isspace(static_cast<unsigned char>(item.back())))
					item.pop_back();

				if (!item.empty())
					out.push_back(std::move(item));
			}
			return out;
		}

		// 解析 Tls_sni_certs："host=cert:key,host2=cert2:key2"
		// 说明：配置文件按 "键 值" 逐 token 读取，值内不能有空格，因此这里不处理空格分隔的写法。
		std::vector<std::pair<std::string, std::pair<std::string, std::string>>>
			parse_sni_certs(const std::string& spec)
		{
			std::vector<std::pair<std::string, std::pair<std::string, std::string>>> out;
			if (spec.empty())
				return out;

			std::size_t pos = 0;
			while (pos <= spec.size())
			{
				const std::size_t comma = spec.find(',', pos);
				const std::string item = spec.substr(
					pos, comma == std::string::npos ? std::string::npos : comma - pos);
				pos = (comma == std::string::npos) ? spec.size() + 1 : comma + 1;
				if (item.empty())
					continue;

				const std::size_t eq = item.find('=');
				if (eq == std::string::npos) {
					LOG_WARN("Tls_sni_certs: skip malformed entry (missing '='): " + item);
					continue;
				}
				const std::string host = item.substr(0, eq);
				const std::string rest = item.substr(eq + 1);
				// 用最后一个 ':' 切分 cert/key，容忍路径里出现 ':' 之外的分隔需求
				const std::size_t colon = rest.rfind(':');
				if (host.empty() || colon == std::string::npos) {
					LOG_WARN("Tls_sni_certs: skip malformed entry (expected host=cert:key): " + item);
					continue;
				}
				const std::string cert = rest.substr(0, colon);
				const std::string key = rest.substr(colon + 1);
				if (cert.empty() || key.empty()) {
					LOG_WARN("Tls_sni_certs: skip entry with empty cert/key: " + item);
					continue;
				}
				out.emplace_back(host, std::make_pair(cert, key));
			}
			return out;
		}
	}

	Server::Server(int argc, char* argv[])
	{
		cfg_.parseArg(argc, argv);
		// 成员 router_ 在默认构造时已建立路由表（handler 捕获的 this 指向成员自身，地址稳定）；
		// 这里只刷新配置。切勿改成 router_ = Router(cfg_)——移动赋值会让 handler 内 this 悬垂
		router_.apply_config(cfg_);
	}

	Server::~Server()
	{
		// 收尾顺序（很关键，改动前请先读这段）：
		//   1) stop()：停止 accept、置探活退出标志、通知所有会话收尾（幂等）
		//   2) join 探活线程与 io 线程：两者都可能正在使用连接池/Redis
		//   3) 回收业务线程池：等在飞业务任务结束——它们可能在结束时 post 回 io_context，
		//      因此必须在 io_context 析构（第 4 步）之前完成
		//   4) 销毁 transport：io_context 析构会丢弃残留 handler 并销毁会话对象
		stop();
		if (probe_thread_.joinable())
			probe_thread_.join();
		if (transport_)
			transport_->join_threads();
		if (pool_)
			pool_.reset();
		transport_.reset();
	}

	TransportOptions Server::build_transport_options() const
	{
		TransportOptions opts;

		opts.http.enable = cfg_.http_enable;
		opts.http.address = cfg_.listen_address;
		opts.http.port = static_cast<unsigned short>(cfg_.port);

		opts.https.enable = cfg_.https_enable;
		opts.https.address = cfg_.listen_address;
		opts.https.port = static_cast<unsigned short>(cfg_.https_port);

		opts.tls.enable = cfg_.https_enable;   // TLS 由 HTTPS 监听器一并启用
		opts.tls.cert_file = cfg_.tls_cert_file;
		opts.tls.key_file = cfg_.tls_key_file;
		opts.tls.ca_file = cfg_.tls_ca_file;
		opts.tls.auto_self_signed = cfg_.tls_auto_self_signed;
		opts.tls.require_client_cert = cfg_.tls_require_client_cert;
		opts.tls.verify_depth = cfg_.tls_verify_depth;
		opts.tls.min_version = cfg_.tls_min_version;
		opts.tls.ciphers = cfg_.tls_ciphers;
		opts.tls.ciphersuites = cfg_.tls_ciphersuites;
		opts.tls.session_tickets = cfg_.tls_session_tickets;
		opts.tls.session_ticket_count = cfg_.tls_session_ticket_count;
		opts.tls.session_id_context = cfg_.tls_session_id_context;
		opts.tls.alpn = cfg_.tls_alpn;
		opts.tls.sni_certs = parse_sni_certs(cfg_.tls_sni_certs);

		opts.allow_peers = cfg_.allow_peers;
		opts.max_connections = static_cast<std::size_t>(cfg_.max_listening > 0 ? cfg_.max_listening : 5000);
		opts.body_limit = static_cast<std::uint64_t>(cfg_.body_limit_bytes > 0 ? cfg_.body_limit_bytes : 65536);
		opts.header_limit = static_cast<std::uint64_t>(cfg_.header_limit_bytes > 0 ? cfg_.header_limit_bytes : 8192);
		opts.idle_timeout_seconds = cfg_.time_out > 0 ? cfg_.time_out : 60;
		opts.request_timeout_seconds = cfg_.request_timeout_seconds > 0 ? cfg_.request_timeout_seconds : 30;
		opts.handshake_timeout_seconds = cfg_.tls_handshake_timeout_seconds > 0 ? cfg_.tls_handshake_timeout_seconds : 10;
		opts.shutdown_timeout_seconds = cfg_.shutdown_timeout_seconds > 0 ? cfg_.shutdown_timeout_seconds : 10;
		opts.shutdown_drain_grace_ms = cfg_.shutdown_drain_grace_ms > 0 ? cfg_.shutdown_drain_grace_ms : 500;
		opts.io_threads = cfg_.resolvedIoThreads();
		opts.redirect_http_to_https = cfg_.http_redirect_to_https;

		// 监听器行为
		opts.listen_backlog = cfg_.listen_backlog;
		opts.listen_dual_stack = cfg_.listen_dual_stack;
		opts.tcp_keepalive = cfg_.tcp_keepalive;

		// 响应头（由会话补齐，避免品牌名/追踪头名硬编码在协议逻辑里）
		opts.server_header = cfg_.server_header;
		opts.default_content_type = cfg_.default_content_type;
		opts.request_id_header = cfg_.request_id_header;
		// 快路径判定：透传给会话，命中时在 io 线程内直接处理而不进业务线程池
		// （handler 捕获 this：Server 的生命周期长于 transport_，析构顺序见 ~Server）
		opts.inline_handler = [this](const HttpRequest& req) { return this->is_inline_request(req); };

		return opts;
	}

	bool Server::init()
	{
		// 初始化日志（根据配置选择同步/异步）；log_type=1 表示异步
		LOG_INIT(cfg_.log_type, cfg_.log_buffer_size, cfg_.log_queue_size, cfg_.log_row_max,
				 cfg_.log_path, cfg_.log_row_flush, cfg_.log_async_retry_ms);
		LOG_INFO("Server init: log initialized.");
		MetricsRegistry::instance().set_liveness(true);
		MetricsRegistry::instance().set_readiness(false);
		MetricsRegistry::instance().set_log_every_n(cfg_.metrics_log_every_n);

		// 快路径前缀（/healthz、/metrics 等）：这些端点在 io 线程内直接处理，不进业务线程池
		inline_prefixes_ = split_csv(cfg_.health_fast_path_prefixes);
		if (!inline_prefixes_.empty())
		{
			std::string joined;
			for (const auto& p : inline_prefixes_)
				joined += (joined.empty() ? "" : ",") + p;
			LOG_INFO("Server init: inline (pool-bypassing) request prefixes: " + joined);
		}

		// 初始化jwt密钥
		if (cfg_.jwt_secret.empty() || cfg_.jwt_secret.size() < 32)
		{
			LOG_ERR("JWT secret is empty or too weak (must be >=32 chars), please update config.");
			return false;
		}

		// CORS 来源白名单：环境变量优先（生产收紧为具体前端域名）
		{
			const char* env_cors = ::getenv("TINYSERVER_CORS_ORIGIN");
			if (env_cors && ::strlen(env_cors) > 0) {
				cfg_.cors_origin = std::string(env_cors);
				LOG_INFO("CORS origin loaded from environment variable.");
			}
		}

		// Redis（默认启用）：优先读环境变量 TINYSERVER_REDIS_URI，否则用配置 Redis_uri。
		// 连接失败不致命：enabled()=false，业务自动降级（缓存穿透 DB、限流/去重回退进程内）。
		{
			const char* env_redis = ::getenv("TINYSERVER_REDIS_URI");
			const std::string uri = (env_redis && ::strlen(env_redis) > 0)
				? std::string(env_redis)
				: (cfg_.redis_uri.empty() ? std::string("tcp://127.0.0.1:6379") : cfg_.redis_uri);
			redis_store::init(uri, static_cast<std::size_t>(cfg_.redis_pool_size > 0 ? cfg_.redis_pool_size : 8));
		}

		// 初始化数据库连接池（全部连接参数与重试策略来自 Config）
		connInit(cfg_);
		LOG_INFO("Server init: connection pool initialized.");

		try
		{
			// 初始化业务线程池（执行阻塞的 SQL/Redis）；队列容量同样来自配置（背压阈值）
			pool_ = std::make_unique<ThreadPool>(
				static_cast<size_t>(cfg_.thread_num),
				static_cast<size_t>(cfg_.thread_pool_queue_size > 0 ? cfg_.thread_pool_queue_size : 100));
			LOG_INFO("Server init: thread pool initialized.");

			// 初始化 Asio 传输层（HTTP / HTTPS 监听、TLS 上下文、信号处理）
			// handler 捕获 this：Server 的生命周期长于 transport_，且析构顺序已保证（见 ~Server）
			transport_ = std::make_unique<AsioTransport>(
				build_transport_options(),
				*pool_,
				[this](HttpRequest& req, HttpResponse& resp) { this->handle_request(req, resp); });
			transport_->init();
			LOG_INFO("Server init: asio transport initialized.");
		}
		catch (const Err& e)
		{
			LOG_ERR(e.getMessage());
			exit(exit_code = e.getType());
		}
		catch (const std::exception& e)
		{
			LOG_ERR(std::string("Server init failed: ") + e.what());
			return false;
		}

		running_ = true;
		MetricsRegistry::instance().set_readiness(true);
		MetricsRegistry::instance().log_snapshot("startup");

		// 启动依赖探活线程（在连接池/Redis 就绪之后、接受流量之前）
		probe_stop_.store(false, std::memory_order_relaxed);
		probe_thread_ = std::thread([this]() { this->dependency_probe_loop(); });
		LOG_INFO("Server init: dependency probe thread started.");

		LOG_INFO("Server start in process " + std::to_string(::syscall(SYS_gettid)) + ".");
		return true;
	}

	bool Server::is_inline_request(const HttpRequest& req) const
	{
		if (!cfg_.health_fast_path || inline_prefixes_.empty())
			return false;

		const auto target_beast = req.target();
		const std::string_view target(target_beast.data(), target_beast.size());
		for (const auto& prefix : inline_prefixes_)
		{
			if (target.rfind(prefix, 0) == 0)
				return true;
		}
		return false;
	}

	/**
	 * 依赖探活主循环。
	 * 每 200ms 醒来一次（分片睡眠，保证停机时能及时退出）：
	 *   * 采样 MySQL 连接池水位并暴露为指标；
	 *   * 用 try_getConnection() 非阻塞判定"池现在还能不能借到连接"（连续失败达阈值才判 DOWN，防抖）；
	 *   * 按 Redis_probe_interval_seconds 做一次 PING（阻塞调用，但只占本线程）。
	 */
	void Server::dependency_probe_loop()
	{
		using namespace std::chrono;

		const auto tick = milliseconds(200);
		const auto redis_interval = milliseconds(
			(cfg_.redis_probe_interval_seconds > 0 ? cfg_.redis_probe_interval_seconds : 0) * 1000);
		const int fail_threshold = cfg_.dependency_probe_fail_threshold > 0
			? cfg_.dependency_probe_fail_threshold : 3;

		auto last_redis_probe = steady_clock::now() - seconds(3600);
		int db_fail_streak = 0;
		int redis_fail_streak = 0;

		while (!probe_stop_.load(std::memory_order_relaxed))
		{
			std::this_thread::sleep_for(tick);
			if (probe_stop_.load(std::memory_order_relaxed))
				break;

			// ---- MySQL：水位 + 非阻塞可用性 ----
			{
				long idle = 0, in_use = 0, max_conn = 0;
				ConnPool::getInstance().stats(idle, in_use, max_conn);
				MetricsRegistry::instance().set_connpool_stats(idle, in_use, max_conn);

				MYSQL* conn = ConnPool::getInstance().try_getConnection();
				const bool ok = (conn != nullptr);
				if (conn)
					ConnPool::getInstance().giveBack(conn);

				db_fail_streak = ok ? 0 : (db_fail_streak + 1);
				// 只在"恢复正常"或"连续失败达阈值"时更新，避免瞬时抖动导致就绪状态反复跳变
				if (db_fail_streak == 0 || db_fail_streak >= fail_threshold)
					MetricsRegistry::instance().set_db_up(db_fail_streak == 0);
			}

			// ---- Redis：按间隔探活；未启用时按"不拖累就绪"处理（与自动降级设计一致）----
			if (!redis_store::enabled())
			{
				MetricsRegistry::instance().set_redis_up(true);
			}
			else if (redis_interval.count() > 0)
			{
				const auto now = steady_clock::now();
				if (now - last_redis_probe >= redis_interval)
				{
					last_redis_probe = now;
					const bool ok = redis_store::ping();
					redis_fail_streak = ok ? 0 : (redis_fail_streak + 1);
					if (redis_fail_streak == 0 || redis_fail_streak >= fail_threshold)
						MetricsRegistry::instance().set_redis_up(redis_fail_streak == 0);
				}
			}
		}
	}

	void Server::handle_request(HttpRequest& req, HttpResponse& resp)
	{
		// 本函数在业务线程池中执行：可以安全使用阻塞的 MySQL / Redis 调用。
		// 注意：绝不可在此直接写 socket——响应由 HttpSession 在自己的 strand 上发送。
		MetricsRegistry::instance().on_request_started();

		// 跨域处理 (CORS)：各项取值均来自配置（Origin 白名单默认 *，生产用 TINYSERVER_CORS_ORIGIN 收紧）
		resp.set(boost::beast::http::field::access_control_allow_origin, cfg_.cors_origin);
		if (!cfg_.cors_allow_methods.empty())
			resp.set(boost::beast::http::field::access_control_allow_methods, cfg_.cors_allow_methods);
		if (!cfg_.cors_allow_headers.empty())
			resp.set(boost::beast::http::field::access_control_allow_headers, cfg_.cors_allow_headers);
		if (cfg_.cors_max_age > 0)
			resp.set("Access-Control-Max-Age", std::to_string(cfg_.cors_max_age));
		if (!cfg_.cors_expose_headers.empty())
			resp.set("Access-Control-Expose-Headers", cfg_.cors_expose_headers);
		if (cfg_.cors_origin != "*")
			resp.set(boost::beast::http::field::vary, "Origin");

		// 快路径（健康检查/指标）在 io 线程上处理，这里不记 INFO，
		// 避免日志锁与格式化占用事件循环（release 构建下 LOG_DEBUG 为空）
		if (is_inline_request(req))
			LOG_DEBUG("Request(inline): " + std::string(req.method_string()) + " " + std::string(req.target()));
		else
			LOG_INFO("Request: " + std::string(req.method_string()) + " " + std::string(req.target()));

		try
		{
			router_.handle_request(req, resp);
		}
		catch (const std::exception& e)
		{
			LOG_ERR(std::string("Router exception: ") + e.what());
			set_json_error(resp, boost::beast::http::status::internal_server_error, "Internal Server Error");
		}
		catch (...)
		{
			LOG_ERR("Router unknown exception.");
			set_json_error(resp, boost::beast::http::status::internal_server_error, "Internal Server Error");
		}

		// Router 未显式设置状态码时给出兜底（避免 status::unknown 被序列化成非法响应行）
		if (resp.result() == boost::beast::http::status::unknown)
			resp.result(boost::beast::http::status::ok);

		// 方法不允许时按 RFC 7231 必须给出 Allow 头（取值与 CORS 的允许方法保持一致）
		if (resp.result() == boost::beast::http::status::method_not_allowed
			&& resp.find(boost::beast::http::field::allow) == resp.end())
			resp.set(boost::beast::http::field::allow,
					 cfg_.cors_allow_methods.empty() ? "GET, POST, PUT, PATCH, DELETE, OPTIONS"
													 : cfg_.cors_allow_methods);

		MetricsRegistry::instance().on_request_completed(static_cast<unsigned>(resp.result_int()));
	}

	void Server::start()
	{
		if (!running_)
			return;

		LOG_INFO("Server start: launching io threads.");
		transport_->start_threads();

		LOG_INFO("Server start: running the io_context on the calling thread.");
		transport_->run();
		LOG_INFO("Server running: io_context loop exited.");

		transport_->join_threads();
		LOG_INFO("Server running: server is gonna shutdown.");
	}

	void Server::stop()
	{
		// 确保 stop 的幂等性：只有第一次调用会触发关闭流程
		bool expected = false;
		if (!stopping_.compare_exchange_strong(expected, true)) {
			return; // 已经在关闭中
		}
		running_ = false;
		// 先让探活线程退出（它在循环里分片睡眠，最多 200ms 内返回）
		probe_stop_.store(true, std::memory_order_relaxed);
		MetricsRegistry::instance().set_readiness(false);
		MetricsRegistry::instance().set_liveness(false);
		MetricsRegistry::instance().log_snapshot("shutdown");
		if (transport_)
			transport_->stop();
		LOG_WARN("Server stop requested.");
	}
}
