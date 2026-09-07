#include "server.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include "http.hpp"
#include "connection.hpp"
#include "metrics.hpp"
#include "redis_store.hpp"
#include <sstream>
#include <utility>

namespace project
{
	Server::Server(int argc, char* argv[])
	{
		cfg_.parseArg(argc, argv);
		reactor_num_ = cfg_.sub_reactor_num;
		// 成员 router_ 在默认构造时已建立路由表（handler 捕获的 this 指向成员自身，地址稳定）；
		// 这里只刷新配置。切勿改成 router_ = Router(cfg_)——移动赋值会让 handler 内 this 悬垂
		router_.apply_config(cfg_);
	}

	Server::~Server()
	{
		if (pool_)
			pool_.reset();
	}

	bool Server::init()
	{
		// 初始化日志（根据配置选择同步/异步）
		// 约定：log_type=1 表示异步
		LOG_INIT(cfg_.log_type, cfg_.log_buffer_size, cfg_.log_queue_size, cfg_.log_row_max, cfg_.log_path, cfg_.log_row_flush);
		LOG_INFO("Server init: log initialized.");
		MetricsRegistry::instance().set_liveness(true);
		MetricsRegistry::instance().set_readiness(false);


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

		// Redis（默认启用）：优先读环境变量 TINYSERVER_REDIS_URI，否则连接本机 6379。
		// 连接失败不致命：enabled()=false，业务自动降级（缓存穿透 DB、限流/去重回退进程内）。
		{
			const char* env_redis = ::getenv("TINYSERVER_REDIS_URI");
			const std::string uri = (env_redis && ::strlen(env_redis) > 0)
				? std::string(env_redis) : std::string("tcp://127.0.0.1:6379");
			redis_store::init(uri);
		}

		// 初始化数据库连接池（这里使用默认参数，最大连接数使用配置）
		// 说明：项目中`connInit`已提供默认连接信息
		connInit(cfg_.address, cfg_.dbport, cfg_.username, cfg_.passwd, cfg_.dbname, cfg_.sql_num, cfg_.retry);
		LOG_INFO("Server init: connection pool initialized.");

		try
		{
			// 初始化线程池
			pool_ = std::make_unique<ThreadPool>((size_t)cfg_.thread_num);
			LOG_INFO("Server init: thread pool initialized.");

			// 初始化子Reactor
			sub_reactors_.clear();
			sub_reactors_.reserve((size_t)reactor_num_);
			for (int i = 0; i < reactor_num_; ++i)
				sub_reactors_.push_back(std::make_shared<SubReactor>(pool_.get(), cfg_.max_listening, cfg_.time_out));
			LOG_INFO("Server init: subReactor initialized.");

			// 初始化Acceptor与主Reactor
			acceptor_ = std::make_unique<Acceptor>(cfg_.port);

			// 注册子reactor内部连接读写回调逻辑
			auto init_connection = [this](SubReactor* sr, int fd) {
				auto conn = std::make_shared<Connection>(fd);
				conn->set_owner(sr);
				sr->set_callbacks(fd,
					[this, conn](int _fd){ return this->handle_read(conn); },
					[this, conn](int _fd){ return this->handle_write(conn); },
					conn
				);
			};

			main_reactor_ = std::make_unique<MainReactor>(*acceptor_, sub_reactors_, init_connection);
			LOG_INFO("Server init: mainReactor initialized.");
		}
		catch (Err& e)
		{
			LOG_ERR(e.getMessage());
			exit(exit_code = e.getType());
		}
		running_ = true;
		MetricsRegistry::instance().set_readiness(true);
		MetricsRegistry::instance().log_snapshot("startup");
		LOG_INFO("Server start in process " + std::to_string(::syscall(SYS_gettid)) + ".");
		return true;
	}

	bool Server::handle_read(std::shared_ptr<Connection> conn)
	{
		int fd = conn->get_fd();
		LOG_INFO("fd " + std::to_string(fd) + " triggers the read callback.");

		// 读取缓冲区数据
		char buf[8192];
		for (;;)
		{
			int n = ::recv(fd, buf, sizeof(buf), 0);
			if (n > 0)
			{
				conn->append_read_data(buf, n);
				continue;
			}
			if (n == 0)
			{
				// 对端关闭：返回 false 让 reactor 移除并销毁连接
				LOG_WARN("fd " + std::to_string(fd) + " closed by peer.");
				return false;
			}
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break; // 本次边沿的数据已读完
			LOG_WARN("fd " + std::to_string(fd) + " recv error. errno: " + std::to_string(errno));
			return false;
		}

		// 保证同一连接同一时刻至多一个处理任务执行，避免并发竞争
		if (conn->has_pending() && conn->try_acquire_task())
		{
			// 非阻塞投递：任务队列满说明线程池过载，此时若阻塞会把整个 Reactor 拖死，
			// 故快速失败并关闭连接（过载保护，客户端可重试）
			if (!pool_->try_enqueue([this, conn]() { process_request(conn); }))
			{
				LOG_WARN("Thread pool overloaded, close fd " + std::to_string(fd) + " (backpressure).");
				conn->release_task();
				return false;
			}
		}
		else if (conn->has_pending())
		{
			// 任务在飞期间又有新数据到达：ET 下该 EPOLLIN 已消费，任务完成后由 reactor 补投递
			conn->note_busy_data();
		}
		return true;
	}

	void Server::process_request(std::shared_ptr<Connection> conn)
	{
		if (!conn) return;
		std::string raw = conn->take_read_buffer();

		if (!raw.empty())
		{
			HttpRequest& req = conn->req;
			HttpResponse& resp = conn->resp;
			// 重置响应对象，避免上一个请求的残留字段污染本次响应
			resp = HttpResponse{};

			ParseResult rc = parse_http_request(raw, req);
			if (rc == ParseResult::NeedMore)
			{
				// 半包：数据放回缓冲等待更多字节，不发送任何响应
				conn->prepend_read_data(raw);
			}
			else
			{
				// 真正开始处理请求时才计入 inflight（半包等待期不计，保证 started/completed 配对）
				MetricsRegistry::instance().on_request_started();
				bool close_req = false;
				if (rc == ParseResult::Error)
				{
					resp.result(boost::beast::http::status::bad_request);
					resp.body() = "Bad Request";
					close_req = true; // 解析错误：发送 400 后关闭连接
				}
				else
				{
					// 跨域处理 (CORS) - 来源白名单可配置（默认 *，生产用 TINYSERVER_CORS_ORIGIN 收紧）
					resp.set(boost::beast::http::field::access_control_allow_origin, cfg_.cors_origin);
					resp.set(boost::beast::http::field::access_control_allow_methods, "GET, POST, PUT, DELETE, OPTIONS, FETCH");
					resp.set(boost::beast::http::field::access_control_allow_headers, "Content-Type, Authorization");

					LOG_INFO("Request: " + std::string(req.method_string()) + " " + std::string(req.target()));
					router_.handle_request(req, resp);
					
					resp.version(req.version() == 0 ? 11 : req.version());
					// keep-alive 时显式告知连接复用，否则保持 close
					resp.set(boost::beast::http::field::connection, req.keep_alive() ? "keep-alive" : "close");
					close_req = !req.keep_alive();
				}

				// 响应序列化进写缓冲；真正发送由 reactor 线程执行（不在 worker 线程同步 send）
				auto out = serialize_http_response(resp);
				conn->append_write_data(out);
				MetricsRegistry::instance().on_request_completed((unsigned)resp.result_int());
				if (close_req)
					conn->request_close();
			}
		}

		// 把"发送响应 / 关闭连接 / 释放任务标志 / 补投递"统一交给 reactor 线程执行，
		// 保证 fd 的读写与关闭只在 reactor 线程发生，消除 fd 重用与跨线程关闭的竞争
		SubReactor* sr = conn->owner();
		if (!sr)
		{
			conn->release_task();
			return;
		}
		auto self = conn;
		sr->post([this, self]() {
			SubReactor* reactor = self->owner();
			if (!reactor) { self->release_task(); return; }

			reactor->flush_write(self);

			if (self->close_requested())
			{
				// 关闭由 reactor 线程执行
				reactor->remove_fd(self->get_fd());
				return;
			}

			self->release_task();

			// 任务在飞期间有新数据到达（ET 下该 EPOLLIN 已消费）→ 补投递下一个处理任务
			if (self->consume_busy_flag() && self->has_pending() && self->try_acquire_task())
			{
				if (!pool_->try_enqueue([this, self]() { process_request(self); }))
				{
					// 过载：无法继续处理，关闭连接
					self->release_task();
					LOG_WARN("Thread pool overloaded (refill), close fd " + std::to_string(self->get_fd()) + ".");
					reactor->remove_fd(self->get_fd());
				}
			}
		});
	}

	bool Server::handle_write(std::shared_ptr<Connection> conn)
	{
		int fd = conn->get_fd();
		// EPOLLOUT 事件：继续发送写缓冲中未发送完的数据
		LOG_INFO("handle_write triggered: executing writing logic for fd " + std::to_string(fd));
		if (conn->owner())
			conn->owner()->flush_write(conn);
		return true;
	}

	void Server::start()
	{
		if (!running_)
			return;
		LOG_INFO("Server start:ready to start the subreactor threads.");
		// 启动子Reactor线程时，同时绑定这些SubReactor的回调逻辑
		sub_threads_.clear();
		sub_threads_.reserve(sub_reactors_.size());
		for (auto& sr : sub_reactors_)
		{
			// std::thread 直接启动子 Reactor 事件循环
			std::thread t([sr]() { sr->loop(); });
			sub_threads_.push_back(std::move(t));
		}

		LOG_INFO("Server start:sub reactors running.");
		LOG_INFO("Server start:main reactor starts.");
		//进入主reactor的循环函数
		main_reactor_->loop();
		LOG_INFO("Server running:main reactor loop func exits.");

		// 正常情况下loop不返回；若返回则等待线程
		for (auto& t : sub_threads_)
		{
			if (t.joinable()) t.join();
		}
		LOG_INFO("Server running:server is gonna shutdown.");
	}

	void Server::stop()
	{
		// 确保 stop 的幂等性：只有第一次调用会触发关闭流程
		bool expected = false;
		if (!stopping_.compare_exchange_strong(expected, true)) {
			return; // 已经在关闭中
		}
		running_ = false;
		MetricsRegistry::instance().set_readiness(false);
		MetricsRegistry::instance().set_liveness(false);
		MetricsRegistry::instance().log_snapshot("shutdown");
		if (main_reactor_) main_reactor_->stop();
		for (auto& sr : sub_reactors_) if (sr) sr->stop();
		LOG_WARN("Server stop requested.");
	}
}
