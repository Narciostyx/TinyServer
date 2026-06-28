#include "server.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include "http.hpp"
#include "connection.hpp"
#include "metrics.hpp"
#include <sstream>
#include <utility>

namespace project
{
	Server::Server(int argc, char* argv[])
	{
		cfg_.parseArg(argc, argv);
		reactor_num_ = cfg_.sub_reactor_num;
		router_ = Router(cfg_);
	}

	bool Server::init()
	{
		// 1) 初始化日志（根据配置选择同步/异步）
		// 约定：log_type==1 表示异步
		logInit(cfg_.log_type, cfg_.log_buffer_size, cfg_.log_queue_size, cfg_.log_row_max, cfg_.log_path, cfg_.log_row_flush);
		LOG_INFO("Server init: log initialized.");
		MetricsRegistry::instance().set_liveness(true);
		MetricsRegistry::instance().set_readiness(false);

		// 优化点：支持从环境变量注入 JWT_SECRET，且在启动时强制校验密钥强度（必须 >= 32 字符）
		// 环境变量优先于配置文件
		{
			const char* env_jwt = ::getenv("JWT_SECRET");
			if (env_jwt && ::strlen(env_jwt) > 0) {
				cfg_.jwt_secret = std::string(env_jwt);
				LOG_INFO("JWT secret loaded from environment variable.");
			}

			if (cfg_.jwt_secret.empty() || cfg_.jwt_secret.size() < 32) {
				LOG_ERR("JWT secret is empty or too weak (must be >=32 chars). Set JWT_SECRET env or update config.");
				return false;
			}
		}

		// 2) 初始化数据库连接池（这里使用默认参数，最大连接数使用配置）
		// 说明：项目中`connInit`已提供默认连接信息
		connInit(cfg_.address, cfg_.dbport, cfg_.username, cfg_.passwd, cfg_.dbname, cfg_.sql_num, cfg_.retry);
		LOG_INFO("Server init: connection pool initialized.");

		try
		{
			// 3) 初始化线程池（线程数来自配置；最大请求数沿用ThreadPool默认或后续扩展）
			pool_ = std::make_unique<ThreadPool>((size_t)cfg_.thread_num);
			LOG_INFO("Server init: thread pool initialized.");

			// 4) 初始化子Reactor
			sub_reactors_.clear();
			sub_reactors_.reserve((size_t)reactor_num_);
			for (int i = 0; i < reactor_num_; ++i)
				sub_reactors_.push_back(std::make_shared<SubReactor>(pool_.get(), cfg_.max_listening, cfg_.time_out));
			LOG_INFO("Server init: subReactor initialized.");

			// 5) 初始化Acceptor与主Reactor
			acceptor_ = std::make_unique<Acceptor>(cfg_.port);

			// 将为新请求安装把回调注入到子reactor的逻辑
			auto init_connection = [this](SubReactor* sr, int fd) {
				auto conn = std::make_shared<Connection>(fd);
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
		LOG_INFO("Server start.");
		return true;
	}

	bool Server::handle_read(std::shared_ptr<Connection> conn)
	{
		int fd = conn->get_fd();
		LOG_INFO("fd " + std::to_string(fd) + " triggers the read callback.");

		char buf[8192];
		int n = ::recv(fd, buf, sizeof(buf), 0);
		if (n <= 0)
		{
			// (1) 错误处理：检查 errno 获取更多信息
			if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
				return true; // 资源暂时不可用，非致命错误，等待下一次事件
			}
			LOG_WARN("fd " + std::to_string(fd) + " recv error or closed. errno: " + std::to_string(errno));
			// 对端关闭或出错，向SubReactor返回false让其销毁fd。
			// 此时 SubReactor 销毁 callback，这也会自然导致 conn 引用计数清零被释放！
			return false;
		}

		conn->append_read_data(buf, n);
		MetricsRegistry::instance().on_request_started();

		// (2) 闭包投递：确保 std::shared_ptr 在所有路径的安全流转
		pool_->enqueue([this, conn]() {
			std::string raw = conn->take_read_buffer();
			HttpRequest& req = conn->req;
			HttpResponse& resp = conn->resp;

			if (!parse_http_request(raw, req))
			{
				resp.result(boost::beast::http::status::bad_request);
				resp.body() = "Bad Request";
			}
			else
			{
				// (7) 日志记录：请求追踪
				LOG_INFO("Request: " + std::string(req.method_string()) + " " + std::string(req.target()));

				// 跨域处理 (CORS) - 全局配置
				resp.set(boost::beast::http::field::access_control_allow_origin, "*");
				resp.set(boost::beast::http::field::access_control_allow_methods, "GET, POST, PUT, DELETE, OPTIONS, FETCH");
				resp.set(boost::beast::http::field::access_control_allow_headers, "Content-Type, Authorization");
				// (3) HTTP 方法判断与路由处理
				router_.handle_request(req, resp);
			}

			// Ensure response version matches request formulation standard
			resp.version(req.version() == 0 ? 11 : req.version());

			auto out = serialize_http_response(resp);
			::send(conn->get_fd(), out.c_str(), (int)out.size(), 0);
			LOG_INFO("Response sent for fd " + std::to_string(conn->get_fd()) + " status: " + std::to_string((unsigned)resp.result_int()));
			MetricsRegistry::instance().on_request_completed((unsigned)resp.result_int());

			// (6) 关闭连接：增加 Keep-Alive 长标志判定机制
			bool keep_alive = req.keep_alive();

			if (!keep_alive)
			{
				close(conn->get_fd());
			}
		});

		// 返回 true 告诉 Reactor 这个 FD 读事件目前处理完了（并没有断开），继续在这个fd监听后续
		return true;
	}

	bool Server::handle_write(std::shared_ptr<Connection> conn)
	{
		int fd = conn->get_fd();
		// 此处执行真正的写缓冲数据发送逻辑
		LOG_INFO("handle_write triggered: executing writing logic for fd " + std::to_string(fd));

		const auto& write_buffer = conn->write_buffer();
		if (write_buffer.empty()) {
			return true;
		}

		int n = ::send(fd, write_buffer.data(), write_buffer.size(), MSG_NOSIGNAL);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				return true; // 发送缓冲满，等下次
			}
			LOG_WARN("fd " + std::to_string(fd) + " send error or closed. errno: " + std::to_string(errno));
			return false;
		}

		if (n == write_buffer.size()) {
			conn->clear_write_buffer();
			// 如果需要在此处移除 EPOLLOUT 的监听可以借由 SubReactor 接口操作
		} else {
			// 未发送完，可以选择清空已发送的部分
			// 目前简化的做法仅做示例
			conn->clear_write_buffer(); 
		}

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
			Thread t;
			t.start([sr]() { sr->loop(); });
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
