#include "server.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include "http.hpp"
#include "connection.hpp"

namespace project
{
	Server::Server(int argc, char* argv[])
	{
		cfg_.parseArg(argc, argv);
		reactor_num_ = cfg_.sub_reactor_num;
	}

	bool Server::init()
	{
		// 1) 初始化日志（根据配置选择同步/异步）
		// 约定：log_type==1 表示异步
		logInit(cfg_.log_type, cfg_.log_buffer_size, cfg_.log_queue_size, cfg_.log_row_max, cfg_.log_path, cfg_.log_row_flush);
		LOG_INFO("Server init: log initialized.");

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
			{
				sub_reactors_.push_back(std::make_shared<SubReactor>(pool_.get()));
			}
			LOG_INFO("Server init: subReactor initialized.");

			// 5) 初始化Acceptor与主Reactor
			acceptor_ = std::make_unique<Acceptor>(cfg_.port);

			// 将为新请求安装把回调注入到子reactor的逻辑。
			auto init_connection = [this](SubReactor* sr, int fd) {
				auto conn = std::make_shared<Connection>(fd);
				sr->set_callbacks(fd,
					[this, conn](int _fd){ return this->handle_read(conn); },
					[this, conn](int _fd){ return this->handle_write(conn); }
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
		LOG_INFO("Server start.");
		return true;
	}

	bool Server::handle_read(std::shared_ptr<Connection> conn)
	{
		int fd = conn->get_fd();
		LOG_INFO("fd " + std::to_string(fd) + " triggers the read callback.");

		char buf[8192] = { 0 };
		int n = ::recv(fd, buf, sizeof(buf), 0);
		if (n <= 0)
		{
			// 对端关闭或出错，向SubReactor返回false让其销毁fd。
			// 此时 SubReactor 销毁 callback，这也会自然导致 conn 引用计数清零被释放！
			return false;
		}

		conn->append_read_data(buf, n);

		// (1) 将业务逻辑投递到线程池以避免阻塞 Reactor 事件循环
		pool_->enqueue([this, conn]() {
			std::string raw = conn->take_read_buffer();
			HttpRequest& req = conn->req;
			HttpResponse& resp = conn->resp;

			if (!parse_http_request(raw, req))
			{
				resp.status = 400;
				resp.reason = "Bad Request";
				resp.body = "Bad Request";
			}
			else
			{
				// 跨域处理 (CORS) - 全局配置
				resp.headers["Access-Control-Allow-Origin"] = "*"; // 根据需求可改为特定域名
				resp.headers["Access-Control-Allow-Methods"] = "GET, POST, PUT, DELETE, OPTIONS, FETCH";
				resp.headers["Access-Control-Allow-Headers"] = "Content-Type, Authorization";

				// 发起预检请求时直接返回可通行
				if (req.method == "OPTIONS")
				{
					resp.status = 204;
					resp.reason = "No Content";
				}
				// 路由示例：
				else if (req.method == "GET" && req.path == "/ping")
				{
					resp.body = "pong";
				}
				else if (req.method == "GET" && req.path == "/db/query")
				{
					std::string body;
					bool ok = db_query("SELECT 1", [&](MYSQL_RES* res) {
						body = "rows=" + std::to_string((long)mysql_num_rows(res));
					});
					if (!ok)
					{
						resp.status = 500;
						resp.reason = "Internal Server Error";
						resp.body = "db query failed";
					}
					else
					{
						resp.body = body;
					}
				}
				else if (req.method == "POST" && req.path == "/db/execute")
				{
					long affected = 0;
					bool ok = db_execute("SELECT 1", [&](long a) { affected = a; });
					if (!ok)
					{
						resp.status = 500;
						resp.reason = "Internal Server Error";
						resp.body = "db execute failed";
					}
					else
					{
						resp.body = "affected=" + std::to_string(affected);
					}
				}
				else if (req.method == "FETCH")
				{
					// 处理新增的 FETCH 请求
					resp.status = 200;
					resp.body = "FETCH method supported";
				}
				else if (req.path.find("/api/") == 0)
				{
					// 处理所有 /api/ 开头的路由请求
					resp.headers["Content-Type"] = "application/json";
					if (req.method == "GET") {
						resp.status = 200;
						resp.body = R"({"status": "success", "message": "GET API received", "data": {}})";
					}
					else if (req.method == "POST") {
						resp.status = 201; // Created或者200均可
						resp.body = R"({"status": "success", "message": "POST API created", "data": "')" + req.body + R"("})";
					}
					else {
						resp.status = 405;
						resp.reason = "Method Not Allowed";
						resp.body = R"({"error": "Method Not Allowed for API endpoint."})";
					}
				}
				else
				{
					resp.status = 404;
					resp.reason = "Not Found";
					resp.body = "Not Found";
				}
			}

			auto out = resp.to_string();
			::send(conn->get_fd(), out.c_str(), (int)out.size(), 0);
			// 目前因为是短链接模式，在应答完毕后主动关闭连接（由reactor底层在读写均关闭/或者自行关闭触发注销）
			close(conn->get_fd());
		});

		// 返回 true，告诉 Reactor 这个 FD 读事件目前处理完了（并没有断开），继续在这个fd监听后续
		return true;
	}

	bool Server::handle_write(std::shared_ptr<Connection> conn)
	{
		int fd = conn->get_fd();
		// 暂时负责注册逻辑
		// 例如：处理客户端身份注册握手，或者在长连接中负责进行事件状态机流转/发送缓冲区的事件注册
		LOG_INFO("handle_write triggered: executing registration logic for fd " + std::to_string(fd));

		// 模拟执行注册的过程（如DB插入等，实际可以通过异步线程池操作）
		// bool ok = db_execute("INSERT INTO users ...", [](long a) {});

		return true;
	}

	void Server::start()
	{
		if (!running_)
			return;

		// 启动子Reactor线程时，同时绑定用于这些 Reactor 的回调函数逻辑 
		sub_threads_.clear();
		sub_threads_.reserve(sub_reactors_.size());
		for (auto& sr : sub_reactors_)
		{
			// 为了能在Reactor底层中应用回调，我们这里可以做一个拦截（更标准的做法应该由Acceptor在新连接进来时自动将回调绑定到FD）
			// 这里因为SubReactor暴露了set_callbacks，但不知道具体的fd，通常需修改MainReactor或SubReactor内部。
			// 但因为在当前架构中MainReactor的dispatch_只是将fd赋予了子Reactor并触发add_fd。
			// 所以我们需要通过另一种方式让所有的新的fd获得handle_read和handle_write。
			Thread t;
			t.start([sr]() { sr->loop(); });
			sub_threads_.push_back(std::move(t));
		}

		LOG_INFO("Server start: sub reactors running. main reactor entering loop.");
		main_reactor_->loop();

		// 正常情况下loop不返回；若返回则等待线程
		for (auto& t : sub_threads_)
		{
			if (t.joinable()) t.join();
		}
	}

	void Server::stop()
	{
		running_ = false;
		if (main_reactor_) main_reactor_->stop();
		for (auto& sr : sub_reactors_) if (sr) sr->stop();
		LOG_WARN("Server stop requested.");
	}

	bool Server::db_execute(const std::string& sql, std::function<void(long)>&& callback) noexcept
	{
		return ConnPool::getInstance().execute(sql, std::move(callback));
	}
}
