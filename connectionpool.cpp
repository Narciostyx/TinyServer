#include "connectionpool.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

void project::ConnPool::init(std::string address, int port, std::string username, std::string password, std::string dbname, int max_size, bool retry)
{
	addr_ = address;
	port_ = port;
	user_ = username;
	passwd_ = password;
	dbname_ = dbname;
	max_size_ = max_size;
	used_size_ = cur_size_ = 0;
	retry_ = retry;

	LOG_INFO("Start to initialize the connections pool and should have " + std::to_string(max_size_) + " connections.");

	conn_ = new std::list<MYSQL*>;
	if (!conn_)
		throw Err("Initialized failed(Allocate pointer failed and will exit with code 1).", kErrType::Sql_init);

	//初始化数据库连接链表
	for (int i = 0; i < max_size_; ++i)
	{
		MYSQL* conn = nullptr;
		MYSQL* connected = nullptr;

		conn = mysql_init(NULL);
		if (!conn)
		{
			if (retry_)
			{
				LOG_INFO("Failed to initialize MYSQL handle, retry...");
				--i;
				continue;
			}
			destroy();
			throw Err("Initialized failed(Initialize MYSQL* failed and exit with code 1).", kErrType::Sql_init);
		}

		// 设置连接超时，避免连接阶段卡住太久
		unsigned int timeout = 5; // 秒
		mysql_options(conn, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);

		// 连接前即固定客户端字符集为 utf8mb4（握手阶段生效），
		// 避免"先以默认字符集交互、后 set_character_set"的首包窗口
		mysql_options(conn, MYSQL_SET_CHARSET_NAME, "utf8mb4");

		while (true)
		{
			connected = mysql_real_connect(conn,
				addr_.c_str(), user_.c_str(), passwd_.c_str(), dbname_.c_str(),
				port_, 0, 0);
			if (connected)
				if (!mysql_set_character_set(connected, "utf8mb4"))
					break;

			const unsigned int err = mysql_errno(conn);
			LOG_ERR(std::string("Connect failed:") + mysql_error(conn) + ", errno=" + std::to_string(err));

			if (!retry_)
			{
				mysql_close(conn);
				destroy();
				throw Err("Connect failed(won't retry due to the config and exit with code 1).", kErrType::Sql_conn);
			}

			++attempt;
			if (attempt >= kMaxAttempts)
			{
				mysql_close(conn);
				destroy();
				throw Err("Connect failed(Reach max retry attempts and exit with code 1).", kErrType::Sql_conn);
			}

			// 不建议重试的情况：账号/密码/库名错误
			if (err == 1045 || err == 1049)
			{
				mysql_close(conn);
				destroy();
				throw Err("Connect failed(Invalid credentials or database name).", kErrType::Sql_conn);
			}

			// 需要等待后重试的情况：连接数用尽/网络连不上
			bool need_wait = (err == 1040 || err == 1203 || err == 2002 || err == 2003);
			int wait_ms = need_wait ? std::min(1000 * attempt, 10000) : 0;
			if (wait_ms > 0)
			{
				LOG_WARN(std::string("Connect retry will wait ") + std::to_string(wait_ms) + "ms...");
				sleep(wait_ms / 1000);
			}
			else
			{
				LOG_INFO("Connect retry immediately...");
			}
		}

		conn_->push_back(connected);
		++cur_size_;
	}
	// 初始化结束

	sem_ = std::make_unique<std::counting_semaphore<kConnPoolSemMax>>(cur_size_);
	LOG_INFO("Initialize ConnPool finished with " + std::to_string(cur_size_) + " connections.");
}

//获取单个连接
MYSQL* project::ConnPool::getConnection()
{
	//当摧毁连接池线程执行时，从此时拒绝获取任何连接
   {
		std::lock_guard<std::mutex> lock(mutex_);
		if (prepare_destroy_ || destroy_)
		{
			LOG_WARN("Due to ConnPool preparing to destroy, reject the request.");
			return nullptr;
		}
	}

   // 以信号量作为可用连接计数，先等待再取连接，避免对cur_size_的竞态读取
	if (!sem_)
		return nullptr;
	sem_->acquire();

	std::lock_guard<std::mutex> lock(mutex_);
	//如果连接池被摧毁，则返回空指针
    if (prepare_destroy_ || destroy_ || !conn_ || conn_->empty())
	{
		// 理论上不应发生；为保证信号量与队列一致，归还一次
		sem_->release();
		return nullptr;
	}
	MYSQL* conn = conn_->front();
	conn_->pop_front();
	--cur_size_;
	++used_size_;
	return conn;
}

//释放单个连接
void project::ConnPool::releaseConnection(MYSQL* conn)
{
	if (conn == nullptr)
		return;
   bool need_close = false;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		//线程池摧毁线程执行时，不会放入空闲队列，而是直接关闭连接
		if (prepare_destroy_ || destroy_ || conn_ == nullptr)
		{
			need_close = true;
			--used_size_;
			if (used_size_ <= 0)
				cv_.notify_all();
		}
		else
		{
			conn_->push_back(conn);
			++cur_size_;
			--used_size_;
			if (used_size_ <= 0)
				cv_.notify_all();
			sem_->release();
		}
	}
	//关闭连接为单个连接句柄传递，不需要加锁
	if (need_close)
		mysql_close(conn);
}

//销毁所有（存在于链表的）连接
void project::ConnPool::destroy()
{
	LOG_INFO("Prepare to destroy the connection pool.");
	std::unique_lock<std::mutex> lock(mutex_);
	//避免被多次调用
	if (destroy_)
		return;
	prepare_destroy_ = true;
	destroy_ = true;

	//通知等待getConnection的线程（release 唤醒，等待者醒来后会因 destroy_ 退出并归还计数）
	if (sem_)
	{
		for (int i = 0; i < max_size_; ++i)
			sem_->release();
	}

	//等待所有被使用连接释放（wait 期间自动释放 mutex_，满足条件后重获锁）
	cv_.wait(lock, [this] { return used_size_ <= 0; });

	LOG_INFO("Execute the destroy function normally.");
	if (conn_)
	{
		for (MYSQL* conn : *conn_)
		{
			mysql_close(conn);
			--cur_size_;
		}
		conn_->clear();
		delete conn_;
		conn_ = nullptr;
	}
}

//std::string project::ConnPool::escapeString(const std::string& str) {
//	MYSQL* conn = getConnection();
//	if (!conn) {
//		LOG_ERR("Failed to get connection for string escaping.");
//		return "";
//	}
//	std::vector<char> buffer((str.length() * 2) + 1);
//	mysql_real_escape_string(conn, buffer.data(), str.c_str(), str.length());
//	std::string esc_str(buffer.data());
//	releaseConnection(conn);
//	return esc_str;
//}