#include "service.hpp"

#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

#include <boost/json.hpp>
#include <mysql/mysql.h>

#include "redis_store.hpp"

namespace {

	// 文章内容/计数变化后使缓存失效（cache-aside 写路径：先写 DB，再删缓存）。
	// 详情键按文章删除；列表键只清前端默认首页(articles:list:1:100)，其余分页靠短 TTL 自过期。
	void invalidate_article_cache(const std::string& article_id, long affected) {
		if (affected > 0 && project::redis_store::enabled()) {
			project::redis_store::cache_del("article:" + article_id);
			project::redis_store::cache_del("articles:list:1:100");
		}
	}

	// 评论列表缓存失效（发表评论后即时失效；删除评论因无 article_id 上下文，靠 60s TTL 自过期）
	void invalidate_comments_cache(const std::string& article_id) {
		if (project::redis_store::enabled())
			project::redis_store::cache_del("comments:" + article_id);
	}

	// 文章列表首页缓存失效（新文章发布/列表页写路径调用）
	void invalidate_article_list_cache() {
		if (project::redis_store::enabled())
			project::redis_store::cache_del("articles:list:1:100");
	}

} // namespace


namespace project {
	bool DataService::fetch_articles(boost::json::array& out, long limit, long offset) noexcept
	{
		std::string sql = "SELECT a.id, a.title, DATE_FORMAT(a.create_time, '%Y-%m-%d %H:%i:%s'), u.username, a.likes, a.views "
						  "FROM article a LEFT JOIN user u ON a.user_id = u.id "
						  "ORDER BY a.create_time DESC";
		if (limit >= 0)
			sql += " LIMIT " + std::to_string(limit) + " OFFSET " + std::to_string(offset);
		
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

		return db_stmt_rw("SELECT id,role FROM user WHERE username = ? AND password = ? AND is_active = 1", [&](void* arg)
			{
				MYSQL_BIND bind[2] = {};
				// user.id 为 bigint：必须用 MYSQL_TYPE_LONGLONG（8 字节）读取，
				// 用 MYSQL_TYPE_LONG 只写入低 4 字节，id 超过 int 上限时会被截断
				bind[0].buffer_type = MYSQL_TYPE_LONGLONG;
				bind[0].buffer = &user_id;

				bind[1].buffer_type = MYSQL_TYPE_TINY;
				bind[1].buffer = &admin;

				if (mysql_stmt_bind_result(static_cast<MYSQL_STMT*>(arg), bind))
					return;

				if (arg && mysql_stmt_fetch(static_cast<MYSQL_STMT*>(arg)) == 0)
					valid = true;
			}, username, password);
	}

	int DataService::create_user(const std::string& username, const std::string& password) noexcept
	{
		// 1) 用户名查重（预处理 + 参数绑定，防注入）
		bool exists = false;
		{
			db_stmt_rw("SELECT 1 FROM user WHERE username = ? LIMIT 1", [&](void* arg)
				{
					auto stmt = static_cast<MYSQL_STMT*>(arg);
					if (!stmt) return;
					MYSQL_BIND b = {};
					int one = 0;
					b.buffer_type = MYSQL_TYPE_LONG;
					b.buffer = &one;
					if (mysql_stmt_bind_result(stmt, &b)) return;
					if (mysql_stmt_fetch(stmt) == 0) exists = true;
				}, username);
		}
		if (exists)
			return 1;

		// 2) 插入新用户：role=0（普通用户，与登录接口的整数 role 语义一致）
		//    并发同名注册由表上的唯一索引兜底（建议 UNIQUE KEY uk_username(username)）。
		long affected = 0;
		bool ok = db_stmt_rw("INSERT INTO user (username, password, role,is_active, create_time) VALUES (?, ?, 0, 1, NOW())",
			[&](void* arg)
			{
				affected = *(long*)arg;
			}, username, password);

		return (ok && affected > 0) ? 0 : -1;
	}

	bool DataService::create_comment(int article_id, long user_id, const std::string& content, long& affected) noexcept
	{
		affected = 0;
		bool ok = db_stmt_rw("INSERT INTO comment (article_id, user_id, content,created_at) VALUES (?,?,?,NOW())", [&](void* arg)
			{
				affected = *(long*)arg;
			}, article_id, user_id, content);
		if (ok && affected > 0)
			invalidate_comments_cache(std::to_string(article_id));
		return ok;
	}

	bool DataService::create_article(const std::string& title, const std::string& content, long user_id, long& affected) noexcept
	{
		affected = 0;
		bool ok = db_stmt_rw("INSERT INTO article (title, content, user_id,create_time,likes,views) VALUES (?,?,?,NOW(),0,0)", [&](void* arg)
			{
				affected = *(long*)arg;
			}, title, content, user_id);
		// 新文章发布后首页列表缓存失效
		if (ok && affected > 0)
			invalidate_article_list_cache();
		return ok;
	}

	bool DataService::fetch_article_detail(const std::string& article_id, long user_id, boost::json::object& out, bool& found) noexcept
	{
		found = false;

		// 缓存（cache-aside）：仅缓存"未登录"视角（userLiked=false 恒成立，缓存安全）。
		// 登录用户回源 DB，保证 userLiked 实时正确。Redis 未启用时自动穿透。
		if (user_id <= 0 && redis_store::enabled())
		{
			auto hit = redis_store::cache_get("article:" + article_id);
			if (hit.has_value())
			{
				try {
					out = boost::json::parse(*hit).as_object();
					found = true;
					return true;
				} catch (...) { /* 缓存损坏：回源 DB */ }
			}
		}

		bool db_ok = db_stmt_rw("SELECT a.title,a.content,DATE_FORMAT(a.create_time, '%Y-%m-%d %H:%i:%s') AS create_time,u.username,a.likes,a.views,CASE WHEN ul.id IS NOT NULL THEN 1 ELSE 0 END AS user_liked FROM article a LEFT JOIN user u ON a.user_id = u.id LEFT JOIN user_likes ul ON ul.article_id = a.id AND ul.user_id = ? WHERE a.id = ? ",
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

		// 回源成功且为匿名视角 → 回填缓存
		if (db_ok && found && user_id <= 0 && redis_store::enabled())
			redis_store::cache_setex("article:" + article_id, 300, boost::json::serialize(out));

		return db_ok;
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
				// comment.id 为 bigint：用 LONGLONG（8 字节）读取，避免截断
				bind[0].buffer_type = MYSQL_TYPE_LONGLONG;
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
				// COUNT()/SUM() 返回 bigint：用 LONGLONG（8 字节）读取
				bind[0].buffer_type = MYSQL_TYPE_LONGLONG;
				bind[1].buffer_type = MYSQL_TYPE_LONGLONG;
				bind[2].buffer_type = MYSQL_TYPE_LONGLONG;
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

		// 事务需要多条语句在同一连接上执行，故借用连接（而不是逐条 stmt_rw_execute）
		MYSQL* conn = ConnPool::getInstance().borrow();
		if (!conn)
		{
			LOG_ERR("Failed to get connection for toggle_like.");
			return false;
		}

		const int uid = static_cast<int>(user_id);

		// 共享参数绑定
		MYSQL_BIND b_uid = {};
		b_uid.buffer_type = MYSQL_TYPE_LONG;
		b_uid.buffer = const_cast<int*>(&uid);

		MYSQL_BIND b_aid = {};
		b_aid.buffer_type = MYSQL_TYPE_STRING;
		b_aid.buffer = const_cast<char*>(article_id.c_str());
		b_aid.buffer_length = static_cast<unsigned long>(article_id.size());

		// 在借用连接上执行一条预处理语句并关闭；失败打印日志返回 false
		auto exec = [&](const char* sql, MYSQL_BIND* params, int nparams) -> bool {
			MYSQL_STMT* st = mysql_stmt_init(conn);
			if (!st)
			{
				LOG_ERR("toggle_like stmt init failed: " + std::string(mysql_error(conn)));
				return false;
			}
			bool ok = true;
			if (mysql_stmt_prepare(st, sql, (unsigned long)std::strlen(sql)))
			{ LOG_ERR("toggle_like prepare failed: " + std::string(mysql_stmt_error(st))); ok = false; }
			if (ok && nparams > 0 && mysql_stmt_bind_param(st, params))
			{ LOG_ERR("toggle_like bind failed: " + std::string(mysql_stmt_error(st))); ok = false; }
			if (ok && mysql_stmt_execute(st))
			{ LOG_ERR("toggle_like execute failed: " + std::string(mysql_stmt_error(st))); ok = false; }
			mysql_stmt_close(st);
			return ok;
		};

		if (mysql_autocommit(conn, 0))
		{
			LOG_ERR("toggle_like: disable autocommit failed: " + std::string(mysql_error(conn)));
			ConnPool::getInstance().giveBack(conn);
			return false;
		}

		// 1) 锁文章行并确认存在（不存在则回滚，article_found 保持 false）
		{
			const char* sql = "SELECT id FROM article WHERE id = ? FOR UPDATE";
			MYSQL_BIND params[1] = { b_aid };
			MYSQL_STMT* st = mysql_stmt_init(conn);
			bool pre_ok = st != nullptr
				&& mysql_stmt_prepare(st, sql, (unsigned long)std::strlen(sql)) == 0
				&& mysql_stmt_bind_param(st, params) == 0
				&& mysql_stmt_execute(st) == 0;
			if (!pre_ok)
			{
				LOG_ERR("toggle_like: lock article failed: " + std::string(st ? mysql_stmt_error(st) : mysql_error(conn)));
				if (st) mysql_stmt_close(st);
				mysql_rollback(conn);
				mysql_autocommit(conn, 1);
				ConnPool::getInstance().giveBack(conn);
				return false;
			}

			MYSQL_BIND out_bind = {};
			long tmp = 0;
			out_bind.buffer_type = MYSQL_TYPE_LONG;
			out_bind.buffer = &tmp;
			bool exists = false;
			if (mysql_stmt_bind_result(st, &out_bind) == 0 && mysql_stmt_store_result(st) == 0)
			{
				if (mysql_stmt_fetch(st) == 0) exists = true;
			}
			mysql_stmt_close(st);

			if (!exists)
			{
				mysql_rollback(conn);
				mysql_autocommit(conn, 1);
				ConnPool::getInstance().giveBack(conn);
				return true; // 文章不存在
			}
			article_found = true;
		}

		// 2) 查询点赞记录（FOR UPDATE 锁行/间隙，串行化同一用户对同一文章的并发点赞）
		bool liked_before = false;
		{
			const char* sql = "SELECT 1 FROM user_likes WHERE user_id = ? AND article_id = ? FOR UPDATE";
			MYSQL_BIND params[2] = { b_uid, b_aid };
			MYSQL_STMT* st = mysql_stmt_init(conn);
			bool pre_ok = st != nullptr
				&& mysql_stmt_prepare(st, sql, (unsigned long)std::strlen(sql)) == 0
				&& mysql_stmt_bind_param(st, params) == 0
				&& mysql_stmt_execute(st) == 0;
			if (!pre_ok)
			{
				LOG_ERR("toggle_like: query user_likes failed: " + std::string(st ? mysql_stmt_error(st) : mysql_error(conn)));
				if (st) mysql_stmt_close(st);
				mysql_rollback(conn);
				mysql_autocommit(conn, 1);
				ConnPool::getInstance().giveBack(conn);
				return false;
			}

			MYSQL_BIND out_bind = {};
			int one = 0;
			out_bind.buffer_type = MYSQL_TYPE_LONG;
			out_bind.buffer = &one;
			if (mysql_stmt_bind_result(st, &out_bind) == 0 && mysql_stmt_store_result(st) == 0)
			{
				if (mysql_stmt_fetch(st) == 0) liked_before = true;
			}
			mysql_stmt_close(st);
		}

		// 3) 写点赞关系 + 更新计数（同一事务，要么全部成功要么回滚）
		bool write_ok = false;
		{
			int delta = liked_before ? -1 : 1;
			MYSQL_BIND b_delta = {};
			b_delta.buffer_type = MYSQL_TYPE_LONG;
			b_delta.buffer = &delta;

			if (!liked_before)
			{
				MYSQL_BIND ins_params[2] = { b_uid, b_aid };
				write_ok = exec("INSERT INTO user_likes (user_id, article_id) VALUES (?, ?)", ins_params, 2);
			}
			else
			{
				MYSQL_BIND del_params[2] = { b_uid, b_aid };
				write_ok = exec("DELETE FROM user_likes WHERE user_id = ? AND article_id = ?", del_params, 2);
			}

			if (write_ok)
			{
				MYSQL_BIND upd_params[2] = { b_delta, b_aid };
				write_ok = exec("UPDATE article SET likes = likes + ? WHERE id = ?", upd_params, 2);
			}
		}

		if (write_ok)
		{
			if (mysql_commit(conn))
			{
				LOG_ERR("toggle_like: commit failed: " + std::string(mysql_error(conn)));
				mysql_rollback(conn);
				write_ok = false;
			}
			else
			{
				liked_after = !liked_before;
			}
		}
		else
		{
			mysql_rollback(conn);
		}

		mysql_autocommit(conn, 1);

		// 4) 提交成功后查询最新计数
		if (write_ok && article_found)
		{
			const char* sql = "SELECT likes FROM article WHERE id = ?";
			MYSQL_BIND params[1] = { b_aid };
			MYSQL_STMT* st = mysql_stmt_init(conn);
			bool pre_ok = st != nullptr
				&& mysql_stmt_prepare(st, sql, (unsigned long)std::strlen(sql)) == 0
				&& mysql_stmt_bind_param(st, params) == 0
				&& mysql_stmt_execute(st) == 0;
			if (pre_ok)
			{
				MYSQL_BIND out_bind = {};
				out_bind.buffer_type = MYSQL_TYPE_LONG;
				out_bind.buffer = &likes_now;
				if (mysql_stmt_bind_result(st, &out_bind) == 0 && mysql_stmt_store_result(st) == 0)
					mysql_stmt_fetch(st);
			}
			if (st) mysql_stmt_close(st);
		}

		// 提交成功后使详情缓存失效（likes 已变化）
		if (write_ok && article_found)
			invalidate_article_cache(article_id, 1);

		ConnPool::getInstance().giveBack(conn);
		return write_ok;
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
			// views 已变化：使详情缓存失效
			invalidate_article_cache(article_id, 1);
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
		bool ok = db_stmt_rw("UPDATE article SET title=?, content=? WHERE id=? AND user_id=?",
			[&](void* arg) { affected = *(long*)arg; },
			title, content, article_id, user_id);
		if (ok) invalidate_article_cache(article_id, affected);
		return ok;
	}

	bool DataService::update_article_title(const std::string& article_id, long user_id, const std::string& title, long& affected) noexcept
	{
		affected = 0;
		bool ok = db_stmt_rw("UPDATE article SET title=? WHERE id=? AND user_id=?",
			[&](void* arg) { affected = *(long*)arg; },
			title, article_id, user_id);
		if (ok) invalidate_article_cache(article_id, affected);
		return ok;
	}

	bool DataService::update_article_content(const std::string& article_id, long user_id, const std::string& content, long& affected) noexcept
	{
		affected = 0;
		bool ok = db_stmt_rw("UPDATE article SET content=? WHERE id=? AND user_id=?",
			[&](void* arg) { affected = *(long*)arg; },
			content, article_id, user_id);
		if (ok) invalidate_article_cache(article_id, affected);
		return ok;
	}

	bool DataService::delete_article(const std::string& article_id, long user_id, long& affected) noexcept
	{
		affected = 0;
		bool ok = db_stmt_rw("DELETE FROM article WHERE id=? AND user_id=?",
			[&](void* arg) { affected = *(long*)arg; },
			article_id, user_id);
		if (ok) invalidate_article_cache(article_id, affected);
		return ok;
	}

	bool DataService::delete_comment(const std::string& comment_id, long user_id, long& affected) noexcept
	{
		affected = 0;

		// 删除前取一次所属文章 id，仅用于删除成功后令评论列表缓存立即失效。
		// 权限判断不依赖此读取（见下方原子 DELETE），因此即使此处读回异常也只退化为 TTL 过期，不影响删除。
		long comment_article = 0;
		{
			db_stmt_rw("SELECT article_id FROM comment WHERE id = ?",
				[&](void* arg) {
					auto stmt = static_cast<MYSQL_STMT*>(arg);
					if (!stmt) return;
					MYSQL_BIND bind[1] = {};
					bind[0].buffer_type = MYSQL_TYPE_LONGLONG; bind[0].buffer = &comment_article;
					if (mysql_stmt_bind_result(stmt, bind)) return;
					(void)mysql_stmt_fetch(stmt);
				}, comment_id);
		}

		// 权限直接做进单条原子 DELETE：
		//   - c.user_id = ?  ：评论作者本人可删
		//   - a.user_id = ?  ：评论所属文章的作者可删（LEFT JOIN；文章已删则此分支为 NULL 失效）
		// affected > 0 即删除成功；0 表示无匹配（评论不存在或无权限，由调用方回 403）
		bool ok = db_stmt_rw(
			"DELETE c FROM comment c LEFT JOIN article a ON a.id = c.article_id "
			"WHERE c.id = ? AND (c.user_id = ? OR a.user_id = ?)",
			[&](void* arg) { affected = *(long*)arg; },
			comment_id, user_id, user_id);

		// 删除成功 → 评论列表缓存立即失效（删除前已取得所属文章 id）
		if (ok && affected > 0 && comment_article > 0)
			invalidate_comments_cache(std::to_string(comment_article));
		return ok;
	}
}
