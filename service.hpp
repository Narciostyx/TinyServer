#pragma once

#include <functional>
#include <string>

#include <boost/json/fwd.hpp>

#include "connectionpool.hpp"

namespace project {
	class DataService {
	public:
		// 获取文章列表；limit<0 表示不分页（兼容旧调用），否则按 (offset, limit) 分页
		bool fetch_articles(boost::json::array& out, long limit = -1, long offset = 0) noexcept;
		bool authenticate_user(const std::string& username, const std::string& password, long& user_id, bool& admin, bool& valid) noexcept;
		// 注册新用户（role=0 普通用户）。返回 0=成功；1=用户名已存在；-1=数据库错误
		int create_user(const std::string& username, const std::string& password) noexcept;
		bool create_comment(int article_id, long user_id, const std::string& content, long& affected) noexcept;
		bool create_article(const std::string& title, const std::string& content, long user_id, long& affected) noexcept;
		bool fetch_article_detail(const std::string& article_id, long user_id, boost::json::object& out, bool& found) noexcept;
		bool fetch_comments(const std::string& article_id, boost::json::array& out) noexcept;
		bool fetch_user_stats(long user_id, boost::json::object& out) noexcept;
		bool toggle_like(const std::string& article_id, long user_id, long& likes_now, bool& liked_after, bool& article_found) noexcept;
		bool update_view(const std::string& article_id, bool do_inc, long& views_now, bool& found) noexcept;
		bool update_article(const std::string& article_id, long user_id, const std::string& title, const std::string& content, long& affected) noexcept;
		bool update_article_title(const std::string& article_id, long user_id, const std::string& title, long& affected) noexcept;
		bool update_article_content(const std::string& article_id, long user_id, const std::string& content, long& affected) noexcept;
		bool delete_article(const std::string& article_id, long user_id, long& affected) noexcept;
		bool delete_comment(const std::string& comment_id, long user_id, long& affected) noexcept;

	private:
		bool db_query(const std::string& sql, std::function<void(MYSQL_RES*)>&& callback) noexcept
		{
			return ConnPool::getInstance().query(sql, std::move(callback));
		}

		template<typename... Args>
		bool db_stmt_rw(const std::string& sql, std::function<void(void*)>&& callback, Args... args) noexcept
		{
			return ConnPool::getInstance().stmt_rw_execute(sql, callback, args...);
		}
	};
}
