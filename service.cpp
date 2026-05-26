#include "service.hpp"

#include <cstdlib>
#include <utility>
#include <vector>

#include <boost/json.hpp>
#include <mysql/mysql.h>

namespace project {
	bool DataService::db_query(const std::string& sql, std::function<void(MYSQL_RES*)>&& callback) noexcept
	{
		return ConnPool::getInstance().query(sql, std::move(callback));
	}

	bool DataService::fetch_articles(boost::json::array& out) noexcept
	{
		std::string sql = "SELECT a.id, a.title, DATE_FORMAT(a.create_time, '%Y-%m-%d %H:%i:%s'), u.username, a.likes, a.views "
						  "FROM article a LEFT JOIN user u ON a.user_id = u.id";

		return db_query(sql, [&](MYSQL_RES* res) {
			if (!res) {
				return;
			}
			MYSQL_ROW row;
			while ((row = mysql_fetch_row(res))) {
				boost::json::object article;
				article["id"] = row[0] ? std::atol(row[0]) : 0;
				article["title"] = row[1] ? row[1] : "";
				article["publishTime"] = row[2] ? row[2] : "";
				article["author"] = row[3] ? row[3] : "Unknown";
				article["likes"] = row[4];
				article["views"] = row[5];
				out.push_back(article);
			}
		});
	}

	bool DataService::authenticate_user(const std::string& username, const std::string& password, long& user_id, bool& admin, bool& valid) noexcept
	{
		valid = false;
		admin = false;
		user_id = 0;

		return db_stmt_rw("SELECT id,role FROM user WHERE username = ? AND password = ?", [&](void* arg)
			{
				MYSQL_BIND bind[2] = {};
				bind[0].buffer_type = MYSQL_TYPE_LONG;
				bind[0].buffer = &user_id;

				bind[1].buffer_type = MYSQL_TYPE_TINY;
				bind[1].buffer = &admin;

				if (mysql_stmt_bind_result(static_cast<MYSQL_STMT*>(arg), bind))
					return;

				if (arg && mysql_stmt_fetch(static_cast<MYSQL_STMT*>(arg)) == 0)
					valid = true;
			}, username, password);
	}

	bool DataService::create_comment(int article_id, long user_id, const std::string& content, long& affected) noexcept
	{
		affected = 0;
		return db_stmt_rw("INSERT INTO comment (article_id, user_id, content,created_at) VALUES (?,?,?,NOW())", [&](void* arg)
			{
				affected = *(long*)arg;
			}, article_id, user_id, content);
	}

	bool DataService::create_article(const std::string& title, const std::string& content, long user_id, long& affected) noexcept
	{
		affected = 0;
		return db_stmt_rw("INSERT INTO article (title, content, user_id,create_time,likes,views) VALUES (?,?,?,NOW(),0,0)", [&](void* arg)
			{
				affected = *(long*)arg;
			}, title, content, std::to_string(user_id));
	}

	bool DataService::fetch_article_detail(const std::string& article_id, long user_id, boost::json::object& out, bool& found) noexcept
	{
		found = false;

		return db_stmt_rw("SELECT a.title,a.content,DATE_FORMAT(a.create_time, '%Y-%m-%d %H:%i:%s') AS create_time,u.username,a.likes,a.views,CASE WHEN ul.id IS NOT NULL THEN 1 ELSE 0 END AS user_liked FROM article a LEFT JOIN user u ON a.user_id = u.id LEFT JOIN user_likes ul ON ul.article_id = a.id AND ul.user_id = ? WHERE a.id = ? ",
			[&](void* arg)
			{
				auto stmt = static_cast<MYSQL_STMT*>(arg);
				if (!stmt) return;

				char title_buf[256] = {};
				std::vector<char> content_buf(2048);
				char time_buf[64] = {};
				char author_buf[256] = {};
				unsigned long title_len = 0, content_len = 0, time_len = 0, author_len = 0;
				int likes = 0, views = 0;
				bool content_is_null = 0, content_error = 0, liked = 0;

				MYSQL_BIND bind[7] = {};
				bind[0].buffer_type = MYSQL_TYPE_STRING;
				bind[0].buffer = title_buf;
				bind[0].buffer_length = sizeof(title_buf);
				bind[0].length = &title_len;

				bind[1].buffer_type = MYSQL_TYPE_STRING;
				bind[1].buffer = content_buf.data();
				bind[1].buffer_length = content_buf.size();
				bind[1].length = &content_len;
				bind[1].is_null = &content_is_null;
				bind[1].error = &content_error;

				bind[2].buffer_type = MYSQL_TYPE_STRING;
				bind[2].buffer = time_buf;
				bind[2].buffer_length = sizeof(time_buf);
				bind[2].length = &time_len;

				bind[3].buffer_type = MYSQL_TYPE_STRING;
				bind[3].buffer = author_buf;
				bind[3].buffer_length = sizeof(author_buf);
				bind[3].length = &author_len;

				bind[4].buffer_type = MYSQL_TYPE_LONG;
				bind[4].buffer = &likes;

				bind[5].buffer_type = MYSQL_TYPE_LONG;
				bind[5].buffer = &views;

				bind[6].buffer_type = MYSQL_TYPE_TINY;
				bind[6].buffer = &liked;

				if (mysql_stmt_bind_result(stmt, bind))
					return;

				int fetch_result = mysql_stmt_fetch(stmt);

				if (fetch_result == 0 || fetch_result == MYSQL_DATA_TRUNCATED) {
					found = true;
					out["title"] = std::string(title_buf, title_len);
					out["publishTime"] = std::string(time_buf, time_len);
					out["author"] = std::string(author_buf, author_len);
					out["id"] = article_id;
					out["likes"] = likes;
					out["views"] = views;
					out["userLiked"] = liked ? true : false;

					if (!content_is_null) {
						std::string content_str;
						if (content_error) {
							content_buf.resize(content_len);
							bind[1].buffer = content_buf.data();
							bind[1].buffer_length = content_buf.size();

							if (mysql_stmt_fetch_column(stmt, &bind[1], 1, 0) == 0) {
								content_str.assign(content_buf.data(), content_len);
							}
						}
						else {
							content_str.assign(content_buf.data(), content_len);
						}
						out["content"] = content_str;
					}
					else {
						out["content"] = "";
					}
				}
			}, user_id, article_id);
	}

	bool DataService::fetch_comments(const std::string& article_id, boost::json::array& out) noexcept
	{
		return db_stmt_rw("SELECT c.id, u.username, c.content FROM comment c JOIN user u ON c.user_id = u.id WHERE c.article_id=?",
			[&](void* arg) {
				auto stmt = static_cast<MYSQL_STMT*>(arg);
				if (!stmt) return;

				long id = 0;
				char username_buf[256] = {};
				std::vector<char> content_buf(8173);
				unsigned long username_len = 0, content_len = 0;

				MYSQL_BIND bind[3] = {};
				bind[0].buffer_type = MYSQL_TYPE_LONG;
				bind[0].buffer = &id;

				bind[1].buffer_type = MYSQL_TYPE_STRING;
				bind[1].buffer = username_buf;
				bind[1].buffer_length = sizeof(username_buf);
				bind[1].length = &username_len;

				bind[2].buffer_type = MYSQL_TYPE_BLOB;
				bind[2].buffer = content_buf.data();
				bind[2].buffer_length = content_buf.size();
				bind[2].length = &content_len;

				if (mysql_stmt_bind_result(stmt, bind)) return;

				while (mysql_stmt_fetch(stmt) == 0) {
					boost::json::object c;
					c["id"] = id;
					c["author"] = std::string(username_buf, username_len);
					if (content_len > content_buf.size()) {
						content_buf.resize(content_len);
						bind[2].buffer = content_buf.data();
						bind[2].buffer_length = content_buf.size();
						mysql_stmt_fetch_column(stmt, &bind[2], 2, 0);
					}
					c["content"] = std::string(content_buf.data(), content_len);
					out.push_back(c);
				}
			},
			article_id);
	}

	bool DataService::fetch_user_stats(long user_id, boost::json::object& out) noexcept
	{
		return db_stmt_rw("SELECT (SELECT COUNT(*) FROM article WHERE user_id = ?) AS article_count, (SELECT COUNT(*) FROM comment WHERE user_id = ?) AS comment_count, (SELECT SUM(likes) FROM article WHERE user_id = ?) AS total_likes", [&](void* arg)
			{
				auto stmt = static_cast<MYSQL_STMT*>(arg);
				if (!stmt) return;

				long articles = 0, comments = 0, likes = 0;
				MYSQL_BIND bind[3] = {};
				bind[0].buffer_type = MYSQL_TYPE_LONG;
				bind[1].buffer_type = MYSQL_TYPE_LONG;
				bind[2].buffer_type = MYSQL_TYPE_LONG;
				bind[0].buffer = &articles;
				bind[1].buffer = &comments;
				bind[2].buffer = &likes;

				if (mysql_stmt_bind_result(stmt, bind))return;

				if (mysql_stmt_fetch(stmt) == 0)
				{
					out["articleCount"] = articles;
					out["totalLikesReceived"] = likes;
					out["commentCount"] = comments;
				}

			}, user_id, user_id, user_id);
	}

	bool DataService::toggle_like(const std::string& article_id, long user_id, long& likes_now, bool& liked_after, bool& article_found) noexcept
	{
		article_found = false;
		liked_after = false;
		likes_now = 0;

		db_stmt_rw("SELECT likes FROM article WHERE id=?", [&](void* arg) {
			auto stmt = static_cast<MYSQL_STMT*>(arg);
			if (!stmt) return;
			MYSQL_BIND b = {};
			b.buffer_type = MYSQL_TYPE_LONG;
			b.buffer = &likes_now;
			if (mysql_stmt_bind_result(stmt, &b)) return;
			if (mysql_stmt_fetch(stmt) == 0) article_found = true;
			}, article_id);

		if (!article_found) {
			return true;
		}

		bool liked_before = false;
		int one = 0;
		db_stmt_rw("SELECT 1 FROM user_likes WHERE user_id=? AND article_id=? LIMIT 1", [&](void* arg) {
			auto stmt = static_cast<MYSQL_STMT*>(arg);
			if (!stmt) return;
			MYSQL_BIND b = {};
			b.buffer_type = MYSQL_TYPE_LONG;
			b.buffer = &one;
			if (mysql_stmt_bind_result(stmt, &b)) return;
			if (mysql_stmt_fetch(stmt) == 0) liked_before = true;
			}, (int)user_id, article_id);

		if (!liked_before) {
			long affected = 0;
			db_stmt_rw("INSERT INTO user_likes (user_id, article_id) VALUES (?, ?)", [&](void* arg) {
				affected = *(long*)arg;
				}, (int)user_id, article_id);

			if (affected > 0) {
				db_stmt_rw("UPDATE article SET likes=likes+1 WHERE id=?", [](void*) {}, article_id);
			}
		}
		else {
			long affected = 0;
			db_stmt_rw("DELETE FROM user_likes WHERE user_id=? AND article_id=?", [&](void* arg) {
				affected = *(long*)arg;
				}, (int)user_id, article_id);

			if (affected > 0) {
				db_stmt_rw("UPDATE article SET likes=IF(likes>0, likes-1, 0) WHERE id=?", [](void*) {}, article_id);
			}
		}

		liked_after = false;
		one = 0;
		db_stmt_rw("SELECT 1 FROM user_likes WHERE user_id=? AND article_id=? LIMIT 1", [&](void* arg) {
			auto stmt = static_cast<MYSQL_STMT*>(arg);
			if (!stmt) return;
			MYSQL_BIND b = {};
			b.buffer_type = MYSQL_TYPE_LONG;
			b.buffer = &one;
			if (mysql_stmt_bind_result(stmt, &b)) return;
			if (mysql_stmt_fetch(stmt) == 0) liked_after = true;
			}, (int)user_id, article_id);

		likes_now = 0;
		article_found = false;
		db_stmt_rw("SELECT likes FROM article WHERE id=?", [&](void* arg) {
			auto stmt = static_cast<MYSQL_STMT*>(arg);
			if (!stmt) return;
			MYSQL_BIND b = {};
			b.buffer_type = MYSQL_TYPE_LONG;
			b.buffer = &likes_now;
			if (mysql_stmt_bind_result(stmt, &b)) return;
			if (mysql_stmt_fetch(stmt) == 0) article_found = true;
			}, article_id);

		return true;
	}

	bool DataService::update_view(const std::string& article_id, bool do_inc, long& views_now, bool& found) noexcept
	{
		if (do_inc) {
			long affected = 0;
			db_stmt_rw("UPDATE article SET views=views+1 WHERE id=?", [&](void* arg) {
				affected = *(long*)arg;
				}, article_id);
			if (affected <= 0) {
				found = false;
				views_now = 0;
				return true;
			}
		}

		found = false;
		views_now = 0;
		db_stmt_rw("SELECT views FROM article WHERE id=?", [&](void* arg) {
			auto stmt = static_cast<MYSQL_STMT*>(arg);
			if (!stmt) return;
			MYSQL_BIND b = {};
			b.buffer_type = MYSQL_TYPE_LONG;
			b.buffer = &views_now;
			if (mysql_stmt_bind_result(stmt, &b)) return;
			if (mysql_stmt_fetch(stmt) == 0) found = true;
			}, article_id);

		return true;
	}

	bool DataService::update_article(const std::string& article_id, long user_id, const std::string& title, const std::string& content, long& affected) noexcept
	{
		affected = 0;
		return db_stmt_rw("UPDATE article SET title=?, content=? WHERE id=? AND user_id=?",
			[&](void* arg) { affected = *(long*)arg; },
			title, content, article_id, user_id);
	}

	bool DataService::update_article_title(const std::string& article_id, long user_id, const std::string& title, long& affected) noexcept
	{
		affected = 0;
		return db_stmt_rw("UPDATE article SET title=? WHERE id=? AND user_id=?",
			[&](void* arg) { affected = *(long*)arg; },
			title, article_id, user_id);
	}

	bool DataService::update_article_content(const std::string& article_id, long user_id, const std::string& content, long& affected) noexcept
	{
		affected = 0;
		return db_stmt_rw("UPDATE article SET content=? WHERE id=? AND user_id=?",
			[&](void* arg) { affected = *(long*)arg; },
			content, article_id, user_id);
	}

	bool DataService::delete_article(const std::string& article_id, long user_id, long& affected) noexcept
	{
		affected = 0;
		return db_stmt_rw("DELETE FROM article WHERE id=? AND user_id=?",
			[&](void* arg) { affected = *(long*)arg; },
			article_id, user_id);
	}

	bool DataService::delete_comment(const std::string& comment_id, long user_id, long& affected) noexcept
	{
		affected = 0;
		return db_stmt_rw("DELETE FROM comment WHERE id=? AND user_id=?",
			[&](void* arg) { affected = *(long*)arg; },
			comment_id, user_id);
	}
}
