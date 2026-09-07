#ifndef _SERVER_HPP
#define _SERVER_HPP

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "acceptor.hpp"
#include "config.hpp"
#include "connectionpool.hpp"
#include "log.hpp"
#include "reactor.hpp"
#include "threadpool.hpp"
#include "connection.hpp"
#include "router.hpp"

#include <unordered_map>
#include <mutex>

namespace project
{
	/**
	 * Server类：服务器主控类
	 * 负责整合Acceptor、Reactor、线程池等组件，定义请求处理回调并管理启动与停止流程
	 */
	class Server
	{
	public:
		/**
		 * 构造：仅保存配置/资源对象，不启动线程
		 * \param argc 命令行参数个数
		 * \param argv 命令行参数（支持 -p 端口 / -l 日志模式 / -s 连接池大小 / -t 线程数等）
		 */
		explicit Server(int argc, char* argv[]);
		/**
		 * 析构：先停止并回收工作线程
		 * 避免 worker 在子 Reactor 销毁后仍通过 conn->owner() 访问悬垂对象
		 */
		~Server();

		/**
		 * 初始化：日志、数据库连接池、线程池、Reactor与Acceptor
		 * \return true 初始化成功；false 失败（如 JWT 密钥缺失或强度不足）
		 */
		bool init();
		/**
		 * 启动：为每个子Reactor创建线程并进入主Reactor事件循环
		 * 主Reactor循环返回后 join 所有子Reactor线程
		 */
		void start();
		/**
		 * 停止：幂等。第一次调用触发关闭流程，唤醒主/子Reactor退出事件循环
		 */
		void stop();

	private:
		Config cfg_;// 配置
		int reactor_num_;//子Reactor数量
		std::atomic<bool> running_{ false };//运行状态
		// 停止/关机路径具有幂等性保障，第一次调用会触发整个关闭流程
		std::atomic<bool> stopping_{ false };
		std::atomic<int> next_sub_{ 0 };


		std::unique_ptr<ThreadPool> pool_;//线程池
		std::unique_ptr<Acceptor> acceptor_;//连接
		std::unique_ptr<MainReactor> main_reactor_;//主Reactor
		std::vector<std::shared_ptr<SubReactor>> sub_reactors_;//子Reactor指针数组
		std::vector<std::thread> sub_threads_;//子Reactor线程数组（std::thread）

		Router router_{ Config{} }; // 路由表

		/**
		 * 读事件回调（子Reactor线程执行）：循环 recv 至 EAGAIN 收数据入连接缓冲，
		 * 连接无任务在飞时向线程池投递 process_request
		 * \param conn 触发读事件的连接
		 * \return true 继续监听该连接；false 表示连接应被关闭
		 */
		bool handle_read(std::shared_ptr<Connection> conn);
		/**
		 * 写事件回调（子Reactor线程执行）：EPOLLOUT 触发时继续发送连接写缓冲
		 * \param conn 触发写事件的连接
		 * \return true 继续监听；false 表示连接应被关闭
		 */
		bool handle_write(std::shared_ptr<Connection> conn);
		/**
		 * 工作线程执行请求处理：解析 -> 路由 -> 序列化到写缓冲，
		 * 随后通过 conn->owner()->post 交给 reactor 线程发送/关闭
		 * \param conn 待处理的连接（调用前需已成功 try_acquire_task）
		 */
		void process_request(std::shared_ptr<Connection> conn);
	};
}

#endif
