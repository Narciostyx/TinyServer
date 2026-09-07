#ifndef _LOG_HPP
#define _LOG_HPP

#include <atomic>
#include <chrono>
#include <format>
#include <fstream>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <memory>

#include "threadsafe_queue.hpp"
#include "error.hpp"

namespace project
{
	namespace 
	{ 
		const int kSleepTime = 10;//线程休眠时间
	}

	// 抽象的Logger基类
	class Logger {
	public:
		virtual ~Logger() = default;
		virtual void init(bool /*async*/, int /*buffer_size*/, int /*queue_size*/, long /*row_max*/, std::string /*path*/, long /*row_flush*/) = 0;
		virtual void write_log(int level, const std::string& data) = 0;
	};

	// 继承Logger
	class Log:public Logger
	{
	public:
		static Log& getInstance()
		{
			static Log instance;
			return instance;
		}
		//初始化日志
		void init(bool, int, int, long, std::string, long);
		//写入日志
		void write_log(int, const std::string&);

	private:
		using sysclock = std::chrono::system_clock;

		bool is_async_ = false;
		std::atomic<bool> run_{ false }; // 供后台写线程与析构线程同步
		bool reach_full_ = false;
		std::string file_path_;
		std::mutex mutex_;
		//写入文件
		std::ofstream file_;
		ThreadSafeQueue<std::string>* queue_;
		int buffer_size_;
		long row_flush_, row_max_;
		std::atomic<long> row_cnt_ = 0;
		//子线程函数：带 1 秒超时阻塞取队列（有数据立即处理；空闲时低频唤醒，退出信号可及时感知）
		std::function<void(void)> worker_func_ = [this]
			{
				long cnt = 0;
				while (run_.load(std::memory_order_relaxed))
				{
					std::optional result = queue_->popWithTime(1);
					if (result.has_value())
					{
						file_ << result.value();
						++cnt;
						if (cnt == row_flush_)
						{
							file_.flush();
							cnt = 0;
						}
					}
				}
			};
		std::thread write_t_; // 异步日志后台写线程（std::thread）

		enum kLevel :int { INFO = 0, WARNING, ERROR, DEBUG };

		Log() {}
		~Log()
		{
			run_ = false;
			if (is_async_ && write_t_.joinable())
				write_t_.join();
			while (!queue_->empty())
			{
				std::optional result = queue_->pop();
				if (result.has_value())
					file_ << result.value();
			}
			file_ << "[StartInfo]\tThe program exits with code " << exit_code << ".\n";
			file_ << "[StartInfo]\tLog closed.";
			file_.flush();
			file_.close();
			delete queue_;
		}

		void write_async(std::string&);
		void write_sync(std::string&);
		// 获取当前时间
		std::string gettime();
	};

	// Logger 管理：持有抽象基类Logger指针
	namespace LoggerHolder 
	{
		// 当前默认日志类使用Log
		using DefaultLogger = Log;
		inline std::shared_ptr<Logger>& get_logger_ref() {
			// 因为Log设计为单例，不能让shared_ptr对其进行析构，使用空deleter
			static std::shared_ptr<Logger> logger{ &Log::getInstance(), [](Logger*) {} };
			return logger;
		}
		inline void set_logger(std::shared_ptr<Logger> l) { get_logger_ref() = std::move(l); }
		inline std::shared_ptr<Logger> get_logger() { return get_logger_ref(); }
	}

	#define LOG_INIT(flag, buffer_size,queue_size,row_max,path,row_flush) project::LoggerHolder::get_logger()->init(flag, buffer_size, queue_size, row_max, path, row_flush)
	#define LOG_UNEXPECT(str) project::LoggerHolder::get_logger()->write_log(-1,str)
	#define LOG_INFO(str) project::LoggerHolder::get_logger()->write_log(0,str)
	#define LOG_WARN(str) project::LoggerHolder::get_logger()->write_log(1,str)
	#define LOG_ERR(str) project::LoggerHolder::get_logger()->write_log(2,str)
	#ifdef _DEBUG
	#define LOG_DEBUG(str) project::LoggerHolder::get_logger()->write_log(3,str)
	#else
	#define LOG_DEBUG(str)
	#endif

}

#endif
