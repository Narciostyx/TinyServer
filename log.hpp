#ifndef _LOG_HPP
#define _LOG_HPP

#include <atomic>
#include <chrono>
#include <format>
#include <fstream>
#include <filesystem>
#include <functional>
#include <unistd.h>
#include <memory>

#include "threadsafe_queue.hpp"
#include "error.hpp"
#include "thread.hpp"

namespace project
{
	namespace 
	{ 
		const int kSleepTime = 10;//线程休眠时间
	}

	// 抽象的 Logger 接口
	class ILogger {
	public:
		virtual ~ILogger() = default;
		virtual void init(bool /*async*/, int /*buffer_size*/, int /*queue_size*/, long /*row_max*/, std::string /*path*/, long /*row_flush*/) = 0;
		virtual void write_log(int level, const std::string& data) = 0;
	};

	// 现有 Log 实现保留，作为默认适配器后备
	class Log
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

		bool is_async_, run_, reach_full_ = false;
		std::string file_path_;
		Mutex mutex_;
		//写入文件
		std::ofstream file_;
		ThreadSafeQueue<std::string>* queue_;
		int buffer_size_;
		long row_flush_, row_max_;
		std::atomic<long> row_cnt_ = 0;
		//子线程函数
		std::function<void(void)> worker_func_ = [this]
			{
				long cnt = 0;
				while (run_)
				{
					if (!queue_->empty())
					{
						std::optional result = queue_->pop();
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
						else
							::usleep(kSleepTime * 1000);
					}
					else
						::usleep(kSleepTime * 1000);
				}
			};
		Thread write_t_;

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
		std::string gettime();
	};

	// 适配器：将现有 Log 封装为 ILogger
	class DefaultLoggerAdapter : public ILogger {
	public:
		void init(bool a, int b, int c, long d, std::string e, long f) override { Log::getInstance().init(a,b,c,d,e,f); }
		void write_log(int level, const std::string& data) override { Log::getInstance().write_log(level, data); }
	};

	// Logger 管理：全局持有当前 ILogger 实例（默认使用 DefaultLoggerAdapter）
	namespace LoggerHolder {
		inline std::shared_ptr<ILogger>& get_logger_ref() {
			static std::shared_ptr<ILogger> logger = std::make_shared<DefaultLoggerAdapter>();
			return logger;
		}
		inline void set_logger(std::shared_ptr<ILogger> l) { get_logger_ref() = std::move(l); }
		inline std::shared_ptr<ILogger> get_logger() { return get_logger_ref(); }
	}

	// 向后兼容的初始化接口
	inline void logInit(bool flag, int buffer_size, int queue_size, long row_max, std::string path, long row_flush) { LoggerHolder::get_logger()->init(flag, buffer_size, queue_size, row_max, path, row_flush); }

	// 兼容旧宏，但内部透过 ILogger
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
