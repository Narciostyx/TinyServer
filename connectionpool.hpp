#ifndef _CONNECTION_POOL_HPP
#define _CONNECTION_POOL_HPP

#include <mysql/mysql.h>

#include <concepts>
#include <list>

#include "mutex.hpp"
#include "log.hpp"

namespace project
{

	namespace { constexpr int kMaxAttempts = 10; }

	class ConnPool
	{
	public:
		static ConnPool& getInstance() { static ConnPool instance; return instance; }
		void init(std::string, int, std::string, std::string, std::string, int, bool);
		//查询（读操作）函数，参数1为查询语句，参数2为回调函数
		template<typename Func>
			requires std::invocable<Func, MYSQL_RES*>
		bool query(const std::string& sql, Func callback) noexcept
		{
			MYSQL* conn = getConnection();
			if (!conn)
			{
				LOG_ERR("Failed to get the connection.");
				return false;
			}

			if (mysql_query(conn, sql.c_str()))
			{
				LOG_ERR(mysql_error(conn));
				releaseConnection(conn);
				return false;
			}

			MYSQL_RES* res;
			if ((res = mysql_store_result(conn)) == NULL)
			{
				LOG_ERR(mysql_error(conn));
				releaseConnection(conn);
				return false;
			}

			callback(res);

			mysql_free_result(res);
			releaseConnection(conn);
			return true;
		}

		// 写操作接口：用于INSERT/UPDATE/DELETE等，不返回结果集，参数2为影响行数回调
		template<typename Func>
			requires std::invocable<Func, long>
		bool execute(const std::string& sql, Func callback) noexcept
		{
			MYSQL* conn = getConnection();
			if (!conn)
			{
				LOG_ERR("Failed to get the connection.");
				return false;
			}

			if (mysql_query(conn, sql.c_str()))
			{
				LOG_ERR(mysql_error(conn));
				releaseConnection(conn);
				return false;
			}

			long affected = (long)mysql_affected_rows(conn);
			callback(affected);
			releaseConnection(conn);
			return true;
		}

		friend void connInit(std::string, int, std::string, std::string, std::string, int, bool);

		// 提供一个逃逸函数接口，利用池中任一空闲连接完成字符转义
		std::string escapeString(const std::string& str);

	private:
		std::string addr_, user_, passwd_, dbname_;
		Mutex mutex_;
        CondVar cv_;
		Sem* sem_ = nullptr;
		int max_size_, port_;
		int cur_size_;//当前空闲连接数
		int used_size_;//当前使用连接数
		int attempt = 0;
        bool retry_, prepare_destroy_ = false, destroy_ = false;
		std::list<MYSQL*>* conn_ = nullptr;

		ConnPool() {}
		~ConnPool() { destroy(); if (sem_) delete sem_; }

		MYSQL* getConnection();
		void releaseConnection(MYSQL*);
		void destroy();
	};

	//连接池初始化函数
	inline void connInit(std::string address, int port, std::string username, std::string password, std::string dbname, int max_size, bool retry)
	{
		try { ConnPool::getInstance().init(address, port, username, password, dbname, max_size, retry); }
		catch (Err& e)
		{
            LOG_ERR(e.getMessage());
			if (e.getType() == kErrType::Sql_conn && ConnPool::getInstance().conn_->size())
				LOG_INFO("Perhaps because of the limit connections or wrong password...?");
			exit(exit_code = 1);
		}
	}
}

#endif