#include "log.hpp"
#include <ctime>
#include <cstdio>

//获取时间
std::string project::Log::gettime()
{
	thread_local time_t last_sec = 0;
	thread_local char time_buf[64] = {0};

	auto now = sysclock::now();
	time_t t_now = std::chrono::system_clock::to_time_t(now);

	// 每个线程每秒只进行一次复杂的日期格式化
	if (t_now != last_sec) {
		last_sec = t_now;
		std::tm tm_buf;
		localtime_r(&t_now, &tm_buf);
		std::strftime(time_buf, sizeof(time_buf), "%Y.%m.%d_%H.%M.%S", &tm_buf);
	}

	return std::string(time_buf);
}

void project::Log::init(bool flag, int buffer_size, int queue_size, long row_max, std::string path, long row_flush)
{
	LockGuard<> lock(mutex_);
	if (!std::filesystem::exists(path))
		if (!std::filesystem::create_directory(path))
		{
			perror("Create Log directory in this directory failed.");
			exit(exit_code = -1);
		}
	is_async_ = flag;
	buffer_size_ = buffer_size;
	file_path_ = path;
	row_flush_ = row_flush;
	queue_ = new ThreadSafeQueue<std::string>(queue_size);
	row_max_ = row_max;

	std::string filename_prefix = "Log_";
	file_path_ += filename_prefix + gettime() + ".txt";
	file_.open(file_path_);
	if (!file_.is_open())
	{
		perror("Open failed.");
		exit(exit_code = -1);
	}
	file_ << "[StartInfo]\tLog initialization completed.\n";
	file_.flush();
	if (is_async_)
	{
		run_ = true;
      write_t_.start(worker_func_);
	}
}

void project::Log::write_log(int level, const std::string& data)
{
	if (!file_.is_open() || reach_full_)
		return;
	if (row_cnt_++ >= row_max_)
	{
		LockGuard<> lock(mutex_);
		reach_full_ = true;
		while (!queue_->empty())
		{
			std::optional result = queue_->pop();
			if (result.has_value())
				file_ << result.value();
		}
		file_ << "[ConfigErr]\tLog file rows reach to the limit.\n";
		file_.flush();
		return;
	}
	std::string msg, lv;

	switch (level)
	{
	case INFO:
		lv = "[INFO]";
		break;
	case WARNING:
		lv = "[WARNING]";
		break;
	case ERROR:
		lv = "[ERROR]";
		break;
	case DEBUG:
		lv = "[DEBUG]";
	default:
		lv = "[UNEXPECTED]";
		break;
	}
	if (data.size())
		msg = std::format("{:<12}\t{}\t{}\n", lv, gettime(), data);
	else
		msg = std::format("{:<12}\t{}\t{}\n", lv, gettime(), "Empty data...?");

	if (is_async_)
		write_async(msg);
	else
		write_sync(msg);
}

void project::Log::write_async(std::string& data)
{
	LockGuard<> lock(mutex_);
	while (true)
		if (queue_->push(std::move(data)))
			break;
		else
		{
         ::usleep(kSleepTime * 1000);
			continue;
		}
}

void project::Log::write_sync(std::string& data)
{
	LockGuard<> lock(mutex_);
	file_ << data;
	file_.flush();
}