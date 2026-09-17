#ifndef _SERVER_HPP
#define _SERVER_HPP

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "asio_transport.hpp"
#include "config.hpp"
#include "connectionpool.hpp"
#include "http.hpp"
#include "log.hpp"
#include "router.hpp"
#include "threadpool.hpp"

namespace project
{
	/**
	 * Server类：服务器主控类（组合根）
	 *
	 * 组装关系：
	 *   Server
	 *     ├─ ThreadPool（业务线程池：执行阻塞的 MySQL / Redis 调用）
	 *     ├─ AsioTransport（Boost.Asio 传输层：HTTP + HTTPS 监听、TLS、超时、优雅停机）
	 *     │    └─ HttpSession<Stream> × N（每条连接一个，socket 建于独立 strand）
	 *     └─ Router → DataService → ConnPool / redis_store
	 *
	 * 线程模型：
	 *   * 调用 start() 的线程（main）运行 io_context，另起 io_threads-1 个 io 线程；
	 *   * io 线程只做非阻塞的收发/解析/超时，绝不执行 SQL/Redis；
	 *   * 业务处理由 io 线程投递到 ThreadPool，处理完成后 post 回会话 strand 写响应。
	 */
	class Server
	{
	public:
		/**
		 * 构造：仅解析配置并建立路由表，不启动线程、不建立监听
		 * \param argc 命令行参数个数
		 * \param argv 命令行参数（-p 端口 / -S 开启 HTTPS / -P HTTPS 端口 / -C 证书 / -K 私钥 等）
		 */
		explicit Server(int argc, char* argv[]);
		/**
		 * 析构：严格按 "停传输 → join io 线程 → 回收业务线程池 → 销毁 io_context" 的顺序收尾
		 */
		~Server();

		/**
		 * 初始化：日志 → JWT 强度校验 → Redis → MySQL 连接池 → 业务线程池 → Asio 传输层
		 * \return true 成功；false 表示配置不合法（如 JWT 密钥过弱）
		 */
		bool init();
		/**
		 * 启动：起 io 线程并在当前线程运行 io_context（阻塞至停机）
		 */
		void start();
		/**
		 * 停止：幂等。第一次调用触发优雅停机（停止 accept、会话收尾、排空后退出事件循环）
		 */
		void stop();

	private:
		/**
		 * 业务处理回调（**在业务线程池内执行**，可安全使用阻塞调用）
		 * \param req 已解析的请求
		 * \param resp 待填充的响应
		 */
		void handle_request(HttpRequest& req, HttpResponse& resp);

		/**
		 * 判断请求是否属于"快路径"：命中配置前缀（默认 /healthz、/livez、/readyz、/metrics）时，
		 * 由 HttpSession 在 io 线程内直接处理，**不投递业务线程池**。
		 * 目的：业务线程池饱和时探针仍能正常应答，而不是被 503 误判为实例故障。
		 * \param req 已解析的请求
		 * \return true 表示走快路径
		 */
		bool is_inline_request(const HttpRequest& req) const;

		/**
		 * 依赖探活线程主循环：周期性采样 MySQL 连接池水位与非阻塞可用性、Redis 可达性，
		 * 结果写入 MetricsRegistry（/metrics 与 /healthz/ready 都读它）。
		 * 说明：刻意不放在业务线程池里——探活必须在池饱和时依然能跑。
		 */
		void dependency_probe_loop();

		/// 由 Config 映射出传输层配置
		TransportOptions build_transport_options() const;

		Config cfg_;// 配置
		std::atomic<bool> running_{ false };// 运行状态
		std::atomic<bool> stopping_{ false };// 停机幂等标志

		std::unique_ptr<ThreadPool> pool_;// 业务线程池
		std::unique_ptr<AsioTransport> transport_;// Asio 传输层（HTTP/HTTPS）

		Router router_{ Config{} };// 路由表（默认构造即建表，见 router.hpp 的说明）

		std::vector<std::string> inline_prefixes_;// 快路径前缀（init 时由配置解析）
		std::atomic<bool> probe_stop_{ true };// 探活线程退出标志
		std::thread probe_thread_;// 依赖探活线程
	};
}

#endif
