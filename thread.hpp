#ifndef _THREAD_HPP
#define _THREAD_HPP

#include <pthread.h>
#include <tuple>
#include <type_traits>
#include <utility>

#include "error.hpp"

namespace project
{
	// 考虑使用标准库<thread>而非本文件

	/**
	 * 对pthread线程的轻量封装
	 * start：启动线程（仅允许调用一次）
	 * join/detach：等待/分离线程
	 * 析构：若仍joinable则自动detach，避免资源泄露/阻塞析构
	 * 注意：线程函数内的异常不能跨pthread边界传播，因此入口会吞掉所有异常
	 */
	[[deprecated( "Use std::thread instead of project::Thread." )]]
	class Thread
	{
	public:
		Thread() = default;

		Thread(const Thread&) = delete;
		Thread& operator=(const Thread&) = delete;
		Thread(Thread&& other) noexcept
		{
			thread_ = other.thread_;
			started_ = other.started_;
			joined_ = other.joined_;
			other.started_ = false;
			other.joined_ = false;
		}
		Thread& operator=(Thread&& other) noexcept
		{
			if (this != &other)
			{
				if (started_ && !joined_)
					pthread_detach(thread_);
				thread_ = other.thread_;
				started_ = other.started_;
				joined_ = other.joined_;
				other.started_ = false;
				other.joined_ = false;
			}
			return *this;
		}

		// 启动线程：func及其参数会按值保存到payload中
		// 约束：同一个Thread对象只能start一次
		template<typename Func, typename... Args>
		void start(Func&& func, Args&&... args)
		{
			if (started_)
				throw Err("Thread already started.", Thread_wrong);

			using Payload = PayloadImpl<std::decay_t<Func>, std::decay_t<Args>...>;
			Payload* payload = new Payload(std::forward<Func>(func), std::forward<Args>(args)...);

			int rc = pthread_create(&thread_, nullptr, &Thread::entry<Payload>, static_cast<void*>( payload ));
			if (rc != 0)
			{
				delete payload;
				throw Err("Failed to create thread.", Thread_wrong);
			}
			started_ = true;
		}

		// 是否可join
		bool joinable() const noexcept { return started_ && !joined_; }

		// 等待线程结束
		void join()
		{
			if (!joinable())
				return;
			pthread_join(thread_, nullptr);
			joined_ = true;
		}

		// 分离线程（不再允许join）
		void detach()
		{
			if (!joinable())
				return;
			pthread_detach(thread_);
			joined_ = true;
		}

		~Thread()
		{
			if (started_ && !joined_)
				pthread_detach(thread_);
		}

	private:
		pthread_t thread_{};
		bool started_ = false;
		bool joined_ = false;

		//封装调用函数与参数
		template<typename F, typename... A>
		struct PayloadImpl
		{
			F f;
			std::tuple<A...> args;
			PayloadImpl(F&& func, A&&... as) : f(std::forward<F>(func)), args(std::forward<A>(as)...)
			{}
			PayloadImpl(const F& func, A... as) : f(func), args(std::move(as)...) {}
		};

		//实际执行调用
		template<typename Payload>
		static void* entry(void* arg)
		{
			Payload* payload = static_cast<Payload*>( arg );
			try
			{
				std::apply(payload->f, payload->args);
			} catch (...)
			{
				//吞噬所有异常，实际执行代码可能会抛出异常，若代码本身未处理，这里避免异常穿过当前线程边界，调用std::terminate()
			}
			delete payload;
			return nullptr;
		}
	};
}

#endif