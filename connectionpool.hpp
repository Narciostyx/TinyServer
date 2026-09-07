#ifndef _CONNECTION_POOL_HPP
#define _CONNECTION_POOL_HPP

#include <mysql/mysql.h>

#include <concepts>
#include <cstddef>
#include <cstdio>
#include <list>
#include <tuple>
#include <string>
#include <type_traits>
#include <mutex>
#include <condition_variable>
#include <semaphore>
#include <memory>

#include "log.hpp"

/**
 * 数据库默认使用webdatabase，登录用户为webdb，密码为webdb.
 * 数据库创建：
 * CREATE DATABASE `webdatabase`;
 * 
 * 创建article表：
 * CREATE TABLE `article` (
  `id` bigint NOT NULL AUTO_INCREMENT,
  `title` varchar(255) NOT NULL,
  `content` longtext NOT NULL,
  `create_time` datetime NOT NULL,
  `user_id` bigint NOT NULL,
  `likes` int NOT NULL,
  `views` int NOT NULL,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB AUTO_INCREMENT=40 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;
 * 
 * 创建comment表：
 * CREATE TABLE `comment` (
  `id` bigint NOT NULL AUTO_INCREMENT,
  `article_id` bigint NOT NULL,
  `user_id` bigint NOT NULL,
  `content` text NOT NULL,
  `created_at` datetime DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`),
  KEY `idx_article_id` (`article_id`),
  KEY `idx_user_id` (`user_id`)
) ENGINE=InnoDB AUTO_INCREMENT=20 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;
 * 
 * 创建user表：
 * CREATE TABLE `user` (
  `id` bigint NOT NULL AUTO_INCREMENT,
  `username` varchar(50) NOT NULL,
  `password` varchar(255) NOT NULL,
  `is_active` tinyint(1) NOT NULL,
  `create_time` datetime NOT NULL,
  `role` tinyint(1) NOT NULL,
  PRIMARY KEY (`id`),
  UNIQUE KEY `uk_username` (`username`)
) ENGINE=InnoDB AUTO_INCREMENT=1003 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;
 * 
 * 创建user_likes表：
 * CREATE TABLE `user_likes` (
  `id` int NOT NULL AUTO_INCREMENT,
  `user_id` int NOT NULL,
  `article_id` int NOT NULL,
  `created_at` timestamp NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`),
  UNIQUE KEY `user_id` (`user_id`,`article_id`)
) ENGINE=InnoDB AUTO_INCREMENT=25 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;
 */


namespace project
{

	namespace {
		constexpr int kMaxAttempts = 10;
		// std::counting_semaphore 编译期上限。destroy() 会用 release(max_size_) 唤醒所有等待者，
		// 峰值计数约为 初始空闲数(max_size_) + 唤醒次数(max_size_) ≤ 2*3000，故上限取 6000。
		constexpr std::ptrdiff_t kConnPoolSemMax = 6000;
	}

	// 连接池类
	class ConnPool
	{
	public:
		static ConnPool& getInstance() { static ConnPool instance; return instance; }
		void init(std::string, int, std::string, std::string, std::string, int, bool);
		/**
		 * 查询函数
		 * 仅在参数可控的情况下使用该函数
		 * 
		 * \param sql：sql语句
		 * \param callback：回调函数
		 * \return 
		 */
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

		 // 写操作接口：用于INSERT / UPDATE / DELETE等，不返回结果集，参数2为影响行数回调
		/*template<typename Func>
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
		}*/

		// 使用预处理语句执行CRUD操作
		template<typename Func, typename... Args>
			requires std::invocable<Func, void*>
		bool stmt_rw_execute(const std::string& sql,Func cb,Args&&... args) noexcept
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
			// 重要：libmysqlclient 的 mysql_stmt_bind_param 只记录参数指针，真正读取发生在
			// mysql_stmt_execute。因此参数缓冲（尤其是超 SSO 的 std::string 堆内存）必须
			// 存活到 execute 结束——execute 必须位于参数 tuple 的作用域内，否则长字符串会
			// 在 execute 前被析构，客户端读到已释放的内存（现象：绑定打印正确、执行乱码）。
			if constexpr (index > 0)
			{
				// 每个参数按值拷贝进 tuple，使绑定缓冲区与本次执行同生命周期
				auto arg = std::make_tuple(std::decay_t<Args>(args)...);
				MYSQL_BIND bind[index] = {};
				unsigned long lengths[index] = {};

				std::apply([&bind, &lengths, this](auto&&... elems) {
					int idx = 0;
					// 辅助函数，负责绑定预处理语句参数
					auto bind_one = [this, &lengths](MYSQL_BIND& b,int cur, auto& value)
						{
							using T = std::remove_cvref_t<decltype(value)>;
							b.buffer_type = get_mysql_type<T>();
							if constexpr (std::is_same_v<T, std::string>)
							{
								b.buffer = const_cast<char*>(value.c_str());
								b.buffer_length = static_cast<unsigned long>(value.size());
								lengths[cur] = b.buffer_length;
								b.length = &lengths[cur];
							}
							else
							{
								b.buffer = &value;
								b.buffer_length = static_cast<unsigned long>(sizeof(value));
							}
						};
					// 参数包展开
					((bind_one(bind[idx], idx, elems), ++idx), ...);
					}, arg);
				if (mysql_stmt_bind_param(stmt, bind))
				{
					LOG_ERR("Failed to bind param:" + std::string(mysql_error(conn)));
					mysql_stmt_close(stmt);
					releaseConnection(conn);
					return false;
				}

//#ifdef _DEBUG
//				// 打印绑定时刻各字符串参数的字节（hex)
//				{
//					std::string dbg = "stmt=[" + sql + "] params:";
//					for (std::size_t di = 0; di < index; ++di)
//					{
//						if (bind[di].buffer_type == MYSQL_TYPE_STRING && bind[di].buffer)
//						{
//							const unsigned char* p = static_cast<const unsigned char*>(bind[di].buffer);
//							const unsigned long n = bind[di].buffer_length;
//							dbg += " p" + std::to_string(di) + "(len=" + std::to_string(n) + ")=";
//							for (unsigned long k = 0; k < n && k < 48; ++k)
//							{
//								char t[4];
//								std::snprintf(t, sizeof(t), "%02x", p[k]);
//								dbg += t;
//							}
//						}
//					}
//					LOG_DEBUG(dbg);
//				}
//#endif

				// execute 必须在此块内执行（arg 仍存活）
				if (mysql_stmt_execute(stmt))
				{
					LOG_ERR("Failed to execute stmt:" + std::string(mysql_error(conn)));
					mysql_stmt_close(stmt);
					releaseConnection(conn);
					return false;
				}
			}
			else
			{
				if (mysql_stmt_execute(stmt))
				{
					LOG_ERR("Failed to execute stmt:" + std::string(mysql_error(conn)));
					mysql_stmt_close(stmt);
					releaseConnection(conn);
					return false;
				}
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

		// 事务等需要"同一连接连续执行多条语句"的场景：借出连接，使用完毕后务必归还
		// （与 getConnection 语义一致：信号量控制并发，destroy 期间拒绝借出）
		MYSQL* borrow() noexcept { return getConnection(); }
		void giveBack(MYSQL* conn) noexcept { releaseConnection(conn); }

		friend void connInit(std::string, int, std::string, std::string, std::string, int, bool);

		// 逃逸函数接口，利用池中任一空闲连接完成字符转义
		//std::string escapeString(const std::string& str);

	private:
		std::string addr_, user_, passwd_, dbname_;
		std::mutex mutex_;
		std::condition_variable cv_;
		std::unique_ptr<std::counting_semaphore<kConnPoolSemMax>> sem_;
		int max_size_, port_;
		int cur_size_;//当前空闲连接数
		int used_size_;//当前使用连接数
		int attempt = 0;
        bool retry_, prepare_destroy_ = false, destroy_ = false;
		std::list<MYSQL*>* conn_ = nullptr;

		ConnPool() {}
		~ConnPool() { destroy(); } // sem_ 为 unique_ptr，自动释放

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