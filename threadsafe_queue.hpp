#ifndef _THREAD_SAFE_QUEUE_HPP
#define _THREAD_SAFE_QUEUE_HPP

#include <queue>
#include <optional>

#include "mutex.hpp"
#include "alloc.hpp"

/* STL分配器有以下要求：
* 定义类型：value_type，size_type，different_type
* 定义方法：allocate，deallocate，重载运算符==与!=
* 定义不同实例模板分配器类的转化构造函数
*/

namespace project
{
	//基于STL Queue的线程安全队列，禁止拷贝与移动
	template<typename T, typename Allocator = std::allocator<T>>
	class ThreadSafeQueue
	{
	public:
		//构造函数
		//参数：队列元素最大值
		ThreadSafeQueue(int max_size = 1024) :max_size_(max_size) {}

		~ThreadSafeQueue() = default;
		ThreadSafeQueue(const ThreadSafeQueue&) = delete;
		ThreadSafeQueue& operator==(const ThreadSafeQueue&) = delete;
		ThreadSafeQueue(ThreadSafeQueue&&) = delete;
		ThreadSafeQueue& operator==(ThreadSafeQueue&&) = delete;

		bool empty() { LockGuard<> lock(mutex_); bool ret = size_ == 0 ? true : false; return ret; }
		bool full() { LockGuard<> lock(mutex_); bool ret = size_ == max_size_; return ret; }
		size_t size() { LockGuard<> lock(mutex_); size_t ret = size_; return ret; }

		//压入队列
		bool push(T&& data)
		{
			LockGuard<> lock(mutex_);
			if (size_ == max_size_)
			{
				cond_.signal();
				return false;
			}
			queue_.push(std::move(data));
			size_++;
			cond_.signal();
			return true;
		}
		//弹出队列
		std::optional<T> pop()
		{
			LockGuard<> lock(mutex_);
			while (size_ == 0)
				if (!cond_.wait(mutex_.get()))
					return std::nullopt;
			T ret = std::move(queue_.front());
			queue_.pop();
			size_--;
			return ret;
		}
		//带超时机制的弹出队列
		std::optional<T> popWithTime(int sec)
		{
			LockGuard<> lock(mutex_);
			if (size_ == 0)
			{
				timespec time;
				if (clock_gettime(CLOCK_REALTIME, &time) == -1)
					return std::nullopt;
				time.tv_sec += sec;
				while (size_ == 0)
					if (!cond_.timedwait(mutex_.get(), &time))
						return std::nullopt;
			}
			T ret = std::move(queue_.front());
			queue_.pop();
			size_--;
			return ret;
		}

	private:
		int max_size_;
		int size_ = 0;
		Allocator alloc_;
		Mutex mutex_;
		CondVar cond_;
		std::queue<T, std::deque<T, Allocator>> queue_;
	};
}

#endif