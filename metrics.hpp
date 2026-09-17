#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <sstream>
#include <string>

#include "log.hpp"

namespace project {
	class MetricsRegistry {
	public:
		static MetricsRegistry& instance() {
			static MetricsRegistry inst;
			return inst;
		}

		void set_liveness(bool live) {
			live_.store(live, std::memory_order_relaxed);
		}

		void set_readiness(bool ready) {
			ready_.store(ready, std::memory_order_relaxed);
		}

		/// 每处理 N 个响应打印一次指标快照；0 表示关闭（配置 Metrics_log_every_n）
		void set_log_every_n(int n) {
			log_every_n_.store(n < 0 ? 0 : n, std::memory_order_relaxed);
		}

		bool is_live() const {
			return live_.load(std::memory_order_relaxed);
		}

		/**
		 * 就绪状态 = 启动完成 && 依赖健康。
		 * 依赖状态由 Server 的探活循环维护（DB 连接池是否还能借到连接、Redis 是否可达）；
		 * 未启用 Redis（enabled()==false）时按"不拖累就绪"处理，与"Redis 挂了自动降级"的设计一致。
		 * \return true 可以对外提供服务
		 */
		bool is_ready() const {
			return ready_.load(std::memory_order_relaxed)
				&& db_up_.load(std::memory_order_relaxed)
				&& redis_up_.load(std::memory_order_relaxed);
		}

		// ---------- 依赖探活（由 Server 的探活循环写入）----------

		void set_db_up(bool up) { db_up_.store(up, std::memory_order_relaxed); }
		void set_redis_up(bool up) { redis_up_.store(up, std::memory_order_relaxed); }
		bool is_db_up() const { return db_up_.load(std::memory_order_relaxed); }
		bool is_redis_up() const { return redis_up_.load(std::memory_order_relaxed); }

		/**
		 * 采样 MySQL 连接池水位
		 * \param idle 空闲连接数
		 * \param in_use 已借出连接数
		 * \param max_conn 池容量
		 */
		void set_connpool_stats(long idle, long in_use, long max_conn) {
			connpool_idle_.store(idle, std::memory_order_relaxed);
			connpool_in_use_.store(in_use, std::memory_order_relaxed);
			connpool_max_.store(max_conn, std::memory_order_relaxed);
		}

		void on_request_started() {
			requests_total_.fetch_add(1, std::memory_order_relaxed);
			inflight_requests_.fetch_add(1, std::memory_order_relaxed);
		}

		// ---------- 传输层连接（AsioTransport / HttpSession）----------

		void on_connection_opened() {
			connections_total_.fetch_add(1, std::memory_order_relaxed);
			connections_active_.fetch_add(1, std::memory_order_relaxed);
		}

		void on_connection_closed() {
			const auto before = connections_active_.fetch_sub(1, std::memory_order_relaxed);
			if (before == 0)
				connections_active_.store(0, std::memory_order_relaxed);
		}

		/// 被 Allow_peers 白名单或连接数上限拒绝的连接
		void on_connection_rejected() {
			connections_rejected_.fetch_add(1, std::memory_order_relaxed);
		}

		/// TLS 握手结果（成功/失败），用于观察证书错误与握手风暴
		void on_tls_handshake(bool ok) {
			if (ok) tls_handshakes_ok_.fetch_add(1, std::memory_order_relaxed);
			else tls_handshakes_failed_.fetch_add(1, std::memory_order_relaxed);
		}

		void on_request_completed(unsigned status_code) {
			auto before = inflight_requests_.fetch_sub(1, std::memory_order_relaxed);
			if (before == 0) {
				inflight_requests_.store(0, std::memory_order_relaxed);
			}

			auto current = responses_total_.fetch_add(1, std::memory_order_relaxed) + 1;

			if (status_code >= 500) {
				responses_5xx_.fetch_add(1, std::memory_order_relaxed);
			}
			else if (status_code >= 400) {
				responses_4xx_.fetch_add(1, std::memory_order_relaxed);
			}
			else if (status_code >= 200 && status_code < 300) {
				responses_2xx_.fetch_add(1, std::memory_order_relaxed);
			}

			if (const auto every = log_every_n_.load(std::memory_order_relaxed); every > 0 && current % static_cast<std::uint64_t>(every) == 0) {
				log_snapshot("periodic");
			}
		}

		std::string render_prometheus() const {
			std::ostringstream out;

			const auto up = uptime_seconds();
			const auto live = is_live() ? 1 : 0;
			const auto ready = is_ready() ? 1 : 0;

			out << "# HELP tinyserver_uptime_seconds Process uptime in seconds\n";
			out << "# TYPE tinyserver_uptime_seconds gauge\n";
			out << "tinyserver_uptime_seconds " << up << "\n";

			out << "# HELP tinyserver_process_live Process liveness state\n";
			out << "# TYPE tinyserver_process_live gauge\n";
			out << "tinyserver_process_live " << live << "\n";

			out << "# HELP tinyserver_process_ready Process readiness state\n";
			out << "# TYPE tinyserver_process_ready gauge\n";
			out << "tinyserver_process_ready " << ready << "\n";

			out << "# HELP tinyserver_http_requests_total Total HTTP requests received\n";
			out << "# TYPE tinyserver_http_requests_total counter\n";
			out << "tinyserver_http_requests_total " << requests_total_.load(std::memory_order_relaxed) << "\n";

			out << "# HELP tinyserver_http_responses_total Total HTTP responses sent\n";
			out << "# TYPE tinyserver_http_responses_total counter\n";
			out << "tinyserver_http_responses_total " << responses_total_.load(std::memory_order_relaxed) << "\n";

			out << "# HELP tinyserver_http_inflight_requests In-flight HTTP requests\n";
			out << "# TYPE tinyserver_http_inflight_requests gauge\n";
			out << "tinyserver_http_inflight_requests " << inflight_requests_.load(std::memory_order_relaxed) << "\n";

			out << "# HELP tinyserver_http_responses_by_class_total HTTP responses grouped by status class\n";
			out << "# TYPE tinyserver_http_responses_by_class_total counter\n";
			out << "tinyserver_http_responses_by_class_total{code_class=\"2xx\"} " << responses_2xx_.load(std::memory_order_relaxed) << "\n";
			out << "tinyserver_http_responses_by_class_total{code_class=\"4xx\"} " << responses_4xx_.load(std::memory_order_relaxed) << "\n";
			out << "tinyserver_http_responses_by_class_total{code_class=\"5xx\"} " << responses_5xx_.load(std::memory_order_relaxed) << "\n";

			out << "# HELP tinyserver_connections_total Total accepted transport connections\n";
			out << "# TYPE tinyserver_connections_total counter\n";
			out << "tinyserver_connections_total " << connections_total_.load(std::memory_order_relaxed) << "\n";

			out << "# HELP tinyserver_connections_active Currently open transport connections\n";
			out << "# TYPE tinyserver_connections_active gauge\n";
			out << "tinyserver_connections_active " << connections_active_.load(std::memory_order_relaxed) << "\n";

			out << "# HELP tinyserver_connections_rejected_total Connections rejected by ACL or connection limit\n";
			out << "# TYPE tinyserver_connections_rejected_total counter\n";
			out << "tinyserver_connections_rejected_total " << connections_rejected_.load(std::memory_order_relaxed) << "\n";

			out << "# HELP tinyserver_tls_handshakes_total TLS handshakes grouped by result\n";
			out << "# TYPE tinyserver_tls_handshakes_total counter\n";
			out << "tinyserver_tls_handshakes_total{result=\"ok\"} " << tls_handshakes_ok_.load(std::memory_order_relaxed) << "\n";
			out << "tinyserver_tls_handshakes_total{result=\"failed\"} " << tls_handshakes_failed_.load(std::memory_order_relaxed) << "\n";

			out << "# HELP tinyserver_dependency_up Dependency health seen by the probe loop (db / redis)\n";
			out << "# TYPE tinyserver_dependency_up gauge\n";
			out << "tinyserver_dependency_up{name=\"db\"} " << (db_up_.load(std::memory_order_relaxed) ? 1 : 0) << "\n";
			out << "tinyserver_dependency_up{name=\"redis\"} " << (redis_up_.load(std::memory_order_relaxed) ? 1 : 0) << "\n";

			out << "# HELP tinyserver_connpool_connections MySQL pool connections by state\n";
			out << "# TYPE tinyserver_connpool_connections gauge\n";
			out << "tinyserver_connpool_connections{state=\"idle\"} " << connpool_idle_.load(std::memory_order_relaxed) << "\n";
			out << "tinyserver_connpool_connections{state=\"in_use\"} " << connpool_in_use_.load(std::memory_order_relaxed) << "\n";
			out << "tinyserver_connpool_connections{state=\"max\"} " << connpool_max_.load(std::memory_order_relaxed) << "\n";

			out << "# HELP tinyserver_log_dropped_lines_total Log lines dropped because the async queue was full\n";
			out << "# TYPE tinyserver_log_dropped_lines_total counter\n";
			out << "tinyserver_log_dropped_lines_total " << Log::dropped_lines() << "\n";

			return out.str();
		}

		std::string render_health_json(bool ready_probe) const {
			const bool up = ready_probe ? is_ready() : is_live();
			return std::string("{\"status\":\"") + (up ? "UP" : "DOWN") + "\"}";
		}

		void log_snapshot(const std::string& reason) const {
			LOG_INFO(
				"Metrics snapshot[" + reason + "]: "
				"uptime=" + std::to_string(uptime_seconds()) + "s"
				", live=" + std::to_string(is_live() ? 1 : 0) +
				", ready=" + std::to_string(is_ready() ? 1 : 0) +
				", requests=" + std::to_string(requests_total_.load(std::memory_order_relaxed)) +
				", inflight=" + std::to_string(inflight_requests_.load(std::memory_order_relaxed)) +
				", responses_2xx=" + std::to_string(responses_2xx_.load(std::memory_order_relaxed)) +
				", responses_4xx=" + std::to_string(responses_4xx_.load(std::memory_order_relaxed)) +
				", responses_5xx=" + std::to_string(responses_5xx_.load(std::memory_order_relaxed)) +
				", connections=" + std::to_string(connections_total_.load(std::memory_order_relaxed)) +
				", active=" + std::to_string(connections_active_.load(std::memory_order_relaxed)) +
				", rejected=" + std::to_string(connections_rejected_.load(std::memory_order_relaxed)) +
				", tls_ok=" + std::to_string(tls_handshakes_ok_.load(std::memory_order_relaxed)) +
				", tls_failed=" + std::to_string(tls_handshakes_failed_.load(std::memory_order_relaxed))
			);
		}

	private:
		MetricsRegistry() : start_time_(std::chrono::steady_clock::now()) {}

		std::int64_t uptime_seconds() const {
			return std::chrono::duration_cast<std::chrono::seconds>(
				std::chrono::steady_clock::now() - start_time_
			).count();
		}

		std::chrono::steady_clock::time_point start_time_;
		std::atomic<bool> live_{ true };
		std::atomic<bool> ready_{ false };
		std::atomic<int> log_every_n_{ 100 };

		std::atomic<std::uint64_t> requests_total_{ 0 };
		std::atomic<std::uint64_t> responses_total_{ 0 };
		std::atomic<std::uint64_t> inflight_requests_{ 0 };

		std::atomic<std::uint64_t> responses_2xx_{ 0 };
		std::atomic<std::uint64_t> responses_4xx_{ 0 };
		std::atomic<std::uint64_t> responses_5xx_{ 0 };

		std::atomic<std::uint64_t> connections_total_{ 0 };
		std::atomic<std::uint64_t> connections_active_{ 0 };
		std::atomic<std::uint64_t> connections_rejected_{ 0 };
		std::atomic<std::uint64_t> tls_handshakes_ok_{ 0 };
		std::atomic<std::uint64_t> tls_handshakes_failed_{ 0 };

		// 依赖探活与连接池水位（初值取"乐观"，避免探活首次执行前误报 DOWN）
		std::atomic<bool> db_up_{ true };
		std::atomic<bool> redis_up_{ true };
		std::atomic<long> connpool_idle_{ 0 };
		std::atomic<long> connpool_in_use_{ 0 };
		std::atomic<long> connpool_max_{ 0 };
	};
}
