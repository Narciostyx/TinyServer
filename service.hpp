#pragma once

#include <functional>
#include <string>

#include <boost/json/fwd.hpp>

#include "connectionpool.hpp"

namespace project {
	class DataService {
	public:
		bool fetch_articles(boost::json::array& out) noexcept;
		bool authenticate_user(const std::string& username, const std::string& password, long& user_id, bool& admin, bool& valid) noexcept;
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
		bool db_query(const std::string& sql, std::function<void(MYSQL_RES*)>&& callback) noexcept;

		template<typename... Args>
		bool db_stmt_rw(const std::string& sql, std::function<void(void*)>&& callback, Args... args) noexcept
		{
			return ConnPool::getInstance().stmt_rw_execute(sql, callback, args...);
		}
	};
}
