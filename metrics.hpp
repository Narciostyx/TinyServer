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

		bool is_live() const {
			return live_.load(std::memory_order_relaxed);
		}

		bool is_ready() const {
			return ready_.load(std::memory_order_relaxed);
		}

		void on_request_started() {
			requests_total_.fetch_add(1, std::memory_order_relaxed);
			inflight_requests_.fetch_add(1, std::memory_order_relaxed);
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

			if (current % 100 == 0) {
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
				", responses_5xx=" + std::to_string(responses_5xx_.load(std::memory_order_relaxed))
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

		std::atomic<std::uint64_t> requests_total_{ 0 };
		std::atomic<std::uint64_t> responses_total_{ 0 };
		std::atomic<std::uint64_t> inflight_requests_{ 0 };

		std::atomic<std::uint64_t> responses_2xx_{ 0 };
		std::atomic<std::uint64_t> responses_4xx_{ 0 };
		std::atomic<std::uint64_t> responses_5xx_{ 0 };
	};
}
