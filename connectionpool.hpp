#ifndef _CONNECTION_POOL_HPP
#define _CONNECTION_POOL_HPP

#include <mysql/mysql.h>

#include <concepts>
#include <list>
#include <tuple>
#include <string>
#include <type_traits>

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
		//template<typename Func>
		//	requires std::invocable<Func, long>
		//bool execute(const std::string& sql, Func callback) noexcept
		//{
		//	MYSQL* conn = getConnection();
		//	if (!conn)
		//	{
		//		LOG_ERR("Failed to get the connection.");
		//		return false;
		//	}

		//	if (mysql_query(conn, sql.c_str()))
		//	{
		//		LOG_ERR(mysql_error(conn));
		//		releaseConnection(conn);
		//		return false;
		//	}

		//	long affected = (long)mysql_affected_rows(conn);
		//	callback(affected);
		//	releaseConnection(conn);
		//	return true;
		//}

		//读接口，使用预处理语句
		template<typename Func, typename... Args>
			requires std::invocable<Func, void*>
		bool stmt_rw_execute(const std::string& sql,Func cb,Args... args) noexcept
		{
			MYSQL* conn = getConnection();
			if (!conn)
			{
				LOG_ERR("Failed to get the connection.");
				return false;
			}

			MYSQL_STMT* stmt = mysql_stmt_init(conn);
			if (!stmt)
			{
				LOG_ERR("Failed to initialize stmt:" + std::string(mysql_error(conn)));
				releaseConnection(conn);
				return false;
			}
			if (mysql_stmt_prepare(stmt, sql.c_str(), sql.length()))
			{
				LOG_ERR("Failed to prepare stmt:" + std::string(mysql_error(conn)));
				mysql_stmt_close(stmt);
				releaseConnection(conn);
				return false;
			}

			constexpr std::size_t index = sizeof...(args);
			if constexpr (index > 0)
			{
				auto arg = std::make_tuple(args...);
				MYSQL_BIND bind[index] = {};
             std::apply([&bind, this](auto&&... elems) {
					int idx = 0;
                   auto bind_one = [this](MYSQL_BIND& b, auto& value)
					{
						using T = std::remove_cvref_t<decltype(value)>;
						b.buffer_type = get_mysql_type<T>();
						if constexpr (std::is_same_v<T, std::string>)
						{
							b.buffer = const_cast<char*>(value.c_str());
							b.buffer_length = static_cast<unsigned long>(value.size());
						}
						else
						{
							b.buffer = &value;
							b.buffer_length = static_cast<unsigned long>(sizeof(value));
						}
					};

					((bind_one(bind[idx], elems), ++idx), ...);
					}, arg);
				if (mysql_stmt_bind_param(stmt, bind))
				{
					LOG_ERR("Failed to bind param:" + std::string(mysql_error(conn)));
					mysql_stmt_close(stmt);
					releaseConnection(conn);
					return false;
				}
			}

			if(mysql_stmt_execute(stmt))
			{
				LOG_ERR("Failed to execute stmt:" + std::string(mysql_error(conn)));
				mysql_stmt_close(stmt);
				releaseConnection(conn);
				return false;
			}

			if (mysql_stmt_field_count(stmt))
			{
				if (mysql_stmt_store_result(stmt))
				{
					LOG_ERR("Failed to store stmt result:" + std::string(mysql_error(conn)));
					mysql_stmt_close(stmt);
					releaseConnection(conn);
					return false;
				}
				cb(stmt);
				mysql_stmt_free_result(stmt);
			}
			else
			{
				long affected = mysql_stmt_affected_rows(stmt);
				cb(&affected);
			}

			mysql_stmt_close(stmt);
			releaseConnection(conn);
			return true;
		}

		friend void connInit(std::string, int, std::string, std::string, std::string, int, bool);

		// 提供一个逃逸函数接口，利用池中任一空闲连接完成字符转义
		//std::string escapeString(const std::string& str);

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

		template<typename T>
		constexpr enum_field_types get_mysql_type() {
			if constexpr (std::is_same_v<T, int8_t>) {
				return MYSQL_TYPE_TINY;
			}
			else if constexpr (std::is_same_v<T, int16_t>) {
				return MYSQL_TYPE_SHORT;
			}
			else if constexpr (std::is_same_v<T, int32_t>) {
				return MYSQL_TYPE_LONG;
			}
			else if constexpr (std::is_same_v<T, int64_t>) {
				return MYSQL_TYPE_LONGLONG;
			}
			else if constexpr (std::is_same_v<T, float>) {
				return MYSQL_TYPE_FLOAT;
			}
			else if constexpr (std::is_same_v<T, double>) {
				return MYSQL_TYPE_DOUBLE;
			}
			else if constexpr (std::is_same_v<T, std::string>) {
				return MYSQL_TYPE_STRING;
			}
			else {
				static_assert(std::false_type::value, "Unsupported type");
			}
		}
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