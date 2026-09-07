#ifndef _THREAD_SAFE_QUEUE_HPP
#define _THREAD_SAFE_QUEUE_HPP

#include <queue>
#include <optional>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <chrono>

namespace project
{
	// 基于STL Queue的线程安全队列（std::mutex + std::condition_variable），禁止拷贝与移动
	template<typename T, typename Allocator = std::allocator<T>>
	class ThreadSafeQueue
	{
	public:
		//构造函数
		//参数：队列元素最大值
		ThreadSafeQueue(int max_size = 1024) :max_size_(max_size) {}

		~ThreadSafeQueue() = default;
		ThreadSafeQueue(const ThreadSafeQueue&) = delete;
		ThreadSafeQueue& operator=(const ThreadSafeQueue&) = delete;
		ThreadSafeQueue(ThreadSafeQueue&&) = delete;
		ThreadSafeQueue& operator=(ThreadSafeQueue&&) = delete;

		bool empty() { std::lock_guard<std::mutex> lock(mutex_); return size_ == 0; }
		bool full() { std::lock_guard<std::mutex> lock(mutex_); return size_ == max_size_; }
		size_t size() { std::lock_guard<std::mutex> lock(mutex_); return size_; }

		//压入队列（满则返回 false，语义不变；调用方决定重试或丢弃）
		bool push(T&& data)
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if (size_ == max_size_)
				return false;
			queue_.push(std::move(data));
			size_++;
			cond_.notify_one();
			return true;
		}
		//弹出队列（队列为空则阻塞等待，直到有数据）
		std::optional<T> pop()
		{
			std::unique_lock<std::mutex> lock(mutex_);
			cond_.wait(lock, [this] { return size_ > 0; });
			T ret = std::move(queue_.front());
			queue_.pop();
			size_--;
			return ret;
		}
		//带超时机制的弹出队列（超时返回 std::nullopt）
		std::optional<T> popWithTime(int sec)
		{
			std::unique_lock<std::mutex> lock(mutex_);
			if (!cond_.wait_for(lock, std::chrono::seconds(sec), [this] { return size_ > 0; }))
				return std::nullopt;
			T ret = std::move(queue_.front());
			queue_.pop();
			size_--;
			return ret;
		}

	private:
		int max_size_;
		int size_ = 0;
		Allocator alloc_;
		std::mutex mutex_;
		std::condition_variable cond_;
		std::queue<T, std::deque<T, Allocator>> queue_;
	};
}

#endif
