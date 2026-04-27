#ifndef _MUTEX_SEM_HPP
#define _MUTEX_SEM_HPP

#include <pthread.h>
#include <semaphore.h>
#include <stdexcept>

namespace project
{
	//对POSIX互斥锁的封装
	class Mutex
	{
	public:
		Mutex() { if (pthread_mutex_init(&mutex_, NULL) != 0)throw std::runtime_error("Failed to init the mutex."); }
		~Mutex() { pthread_mutex_destroy(&mutex_); }

		Mutex(const Mutex&) = delete;
		Mutex& operator=(const Mutex&) = delete;
		Mutex(Mutex&&) = delete;
		Mutex& operator=(Mutex&&) = delete;

		bool lock() { return pthread_mutex_lock(&mutex_) == 0; }
		bool unlock() { return pthread_mutex_unlock(&mutex_) == 0; }

		pthread_mutex_t* get() { return &mutex_; }

	private:
		pthread_mutex_t mutex_;
	};

	//针对Mutex类的RAII封装
	template<typename MutexType=Mutex>
	class LockGuard
	{
	public:
		LockGuard(MutexType& mutex) :mutex_(mutex) { if (!mutex_.lock()) throw std::runtime_error("Failed to lock the mutex."); }
		~LockGuard() { mutex_.unlock(); }

		LockGuard(const LockGuard&) = delete;
		LockGuard& operator=(const LockGuard&) = delete;
		LockGuard(LockGuard&&) = delete;
		LockGuard& operator=(LockGuard&&) = delete;

	private:
		MutexType& mutex_;
	};

	//对POSIX线程信号量的封装
	class Sem
	{
	public:
		Sem() { if (sem_init(&sem_, NULL, 0) != 0)throw std::runtime_error("Failed to init the sem."); }
		Sem(int n) { if (sem_init(&sem_, NULL, n) != 0)throw std::runtime_error("Failed to init the sem."); }
		~Sem() { sem_destroy(&sem_); }

		Sem(const Sem&) = delete;
		Sem& operator=(const Sem&) = delete;
		Sem(Sem&&) = delete;
		Sem& operator=(Sem&&) = delete;

		bool wait() { return sem_wait(&sem_) == 0; }
		bool post() { return sem_post(&sem_) == 0; }

	private:
		sem_t sem_;
	};

	//对POSIX线程条件变量的封装
	class CondVar
	{
	public:
		CondVar() { if (pthread_cond_init(&cond_, NULL) != 0)throw std::runtime_error("Failed to init the condition_variable."); }
		~CondVar() { pthread_cond_destroy(&cond_); }

		CondVar(const CondVar&) = delete;
		CondVar& operator=(const CondVar&) = delete;
		CondVar(CondVar&&) = delete;
		CondVar& operator=(CondVar&&) = delete;
		
		bool wait(pthread_mutex_t* mutex) { return pthread_cond_wait(&cond_, mutex) == 0; }
		//带超时机制的等待
		bool timedwait(pthread_mutex_t* mutex, timespec* ts) { return pthread_cond_timedwait(&cond_, mutex, ts) == 0; }
		//通知单个等待线程
		bool signal() { return pthread_cond_signal(&cond_) == 0; }
		//通知所有等待线程
		bool broadcast() { return pthread_cond_broadcast(&cond_) == 0; }

	private:
		pthread_cond_t cond_;
	};
}

#endif