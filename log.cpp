#include "log.hpp"
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <thread>

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

void project::Log::init(bool flag, int buffer_size, int queue_size, long row_max, std::string path, long row_flush, int retry_ms)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!std::filesystem::exists(path))
		if (!std::filesystem::create_directories(path))
		{
			perror("Create Log directory in this directory failed.");
			exit(exit_code = -1);
		}
	is_async_ = flag;
	buffer_size_ = buffer_size;
	file_path_ = path;
	row_flush_ = row_flush;
	retry_ms_ = retry_ms > 0 ? retry_ms : 10;
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
		// 用 std::thread 启动后台写线程（worker_func_ 为捕获 this 的成员函数对象）
		write_t_ = std::thread([this] { worker_func_(); });
	}
}

void project::Log::write_log(int level, const std::string& data)
{
	if (!file_.is_open() || reach_full_)
		return;
	if (row_cnt_++ >= row_max_)
	{
		std::lock_guard<std::mutex> lock(mutex_);
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
		break;
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

/**
 * 异步写入：把整行交给后台写线程。
 *
 * 原实现是"持 mutex_ + usleep(10ms) 忙等直到入队成功"，有两个严重问题：
 *   1) 持着全局日志锁睡觉 => 期间所有线程的日志调用全部阻塞（比日志本身更贵）；
 *   2) 忙等会持续占用调用方线程，日志队列积压时把业务线程也拖住。
 * 现改为：有限次重试（睡眠在锁外），仍失败则丢弃并计数——日志绝不允许拖垮业务。
 */
void project::Log::write_async(std::string& data)
{
    constexpr int kMaxAttempts = 3;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // ThreadSafeQueue::push 在队列满时不做 move，data 仍然有效，可安全重试
            if (queue_->push(std::move(data)))
                return;
        }
        // 注意：睡眠必须在锁外，否则又会变成"持锁等待"
        std::this_thread::sleep_for(std::chrono::milliseconds(retry_ms_));
    }
    dropped_lines_.fetch_add(1, std::memory_order_relaxed);
}

/**
 * 同步写入：直接写文件。
 *
 * 原实现每写一行就 flush() 一次——每行日志等于一次系统调用/落盘，
 * 而且是在持有全局锁的情况下做的。现改为按 row_flush_ 批量 flush
 * （与配置项 log_row_flush 的语义一致），析构与 reach_full_ 路径仍会强制 flush。
 */
void project::Log::write_sync(std::string& data)
{
    std::lock_guard<std::mutex> lock(mutex_);
    file_ << data;
    if (++sync_line_cnt_ >= row_flush_)
    {
        sync_line_cnt_ = 0;
        file_.flush();
    }
}