#ifndef _SERVER_HPP
#define _SERVER_HPP

#include <atomic>
#include <memory>
#include <vector>

#include "thread.hpp"

#include "acceptor.hpp"
#include "config.hpp"
#include "connectionpool.hpp"
#include "log.hpp"
#include "reactor.hpp"
#include "threadpool.hpp"
#include "connection.hpp"

#include <unordered_map>
#include <mutex>

namespace project
{
	// Server类：服务器主控类
	// 负责整合Acceptor、Reactor、线程池等组件
	class Server
	{
	public:
		// 构造：仅保存配置/资源对象，不启动线程
		explicit Server(int argc, char* argv[]);

		// 初始化：日志、数据库连接池、线程池、Reactor与Acceptor
		bool init();
		// 启动：启动子Reactor线程，并进入主Reactor循环
		void start();
		// 停止：目前仅做标志位（后续可扩展为优雅退出）
		void stop();

		// 数据库查询：sql为SELECT等查询语句，callback消费MYSQL_RES*
		template<typename Func>
		bool db_query(const std::string& sql, Func callback) noexcept
		{
			return ConnPool::getInstance().query(sql, std::move(callback));
		}

		// 数据库增删改：sql为INSERT/UPDATE/DELETE等语句，callback返回受影响行数。
		bool db_execute(const std::string& sql, std::function<void(long)>&& callback) noexcept;

	private:
		
		Config cfg_;// 配置
		int reactor_num_;//子Reactor数量
		std::atomic<bool> running_{ false };//运行状态
		std::atomic<int> next_sub_{ 0 };

		
		std::unique_ptr<ThreadPool> pool_;//线程池
		std::unique_ptr<Acceptor> acceptor_;//连接
		std::unique_ptr<MainReactor> main_reactor_;//主Reactor
		std::vector<std::shared_ptr<SubReactor>> sub_reactors_;//子Reactor指针数组
		std::vector<Thread> sub_threads_;//子Reactor线程数组

		// 内部读写回调
		bool on_read(int fd);
		bool on_write(int fd);

		// 为了跨线程及对象传递支持，定义成员级可绑定的包装回调
		bool handle_read(std::shared_ptr<Connection> conn);
		bool handle_write(std::shared_ptr<Connection> conn);
	};
}

#endif
