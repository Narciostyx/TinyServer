#include "router.hpp"
#include <sstream>
#include <boost/json.hpp>
#include <regex>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <cctype>

namespace json = boost::json;

namespace project {
    Router::Router() {
        init_routes();
    }

    // 简单检查用户登录认证
    static long check_auth_and_get_user_id(auto& req, auto& resp) {
        auto auth_it = req.find(boost::beast::http::field::authorization);
        if (auth_it == req.end() || !auth_it->value().starts_with("Bearer ")) {
            return 0;
        }
        std::string tokenStr = auth_it->value().substr(7);
        if (tokenStr.find("mock_token_user_") == 0) {
            return std::atol(tokenStr.substr(16).c_str());
        }
        return 0;
    }

    // 返回错误信息json
    static void set_json_err(auto& resp, boost::beast::http::status status, const std::string& msg) {
        resp.result(status);
        json::object err_obj;
        err_obj["message"] = msg;
        resp.body() = json::serialize(err_obj);
    }

    static bool is_numeric(std::string_view s) {
        if (s.empty()) return false;
        for (unsigned char ch : s) {
            if (!std::isdigit(ch)) return false;
        }
        return true;
    }

    static std::string get_client_key(const boost::beast::http::request<boost::beast::http::string_body>& req) {
        auto it = req.find("X-Forwarded-For");
        if (it != req.end() && !it->value().empty())
            return std::string(it->value());

        it = req.find("X-Real-IP");
        if (it != req.end() && !it->value().empty())
            return std::string(it->value());

        it = req.find(boost::beast::http::field::authorization);
        if (it != req.end() && !it->value().empty())
            return std::string(it->value());

        return "anon";
    }

    //初始化路由表
    void Router::init_routes() {
        // --- GET ---
        // GET /api/ping
        get_routes_["/ping"] = [](auto& req, auto& resp) {
            resp.result(boost::beast::http::status::ok);
            resp.body() = "pong";
        };

        // 获取文章列表
        // GET /api/articles
        get_routes_["/articles"] = [this](auto& req, auto& resp) {
            resp.set(boost::beast::http::field::content_type, "application/json");
            json::array articles_list;

            std::string sql = "SELECT a.id, a.title, DATE_FORMAT(a.create_time, '%Y-%m-%d %H:%i:%s'), u.username, a.likes, a.views "
                              "FROM article a LEFT JOIN user u ON a.user_id = u.id";
            
            db_query(sql, [&](MYSQL_RES* res) {
                if (res) {
                    MYSQL_ROW row;
                    while ((row = mysql_fetch_row(res))) {
                        json::object article;
                        article["id"] = row[0] ? std::atol(row[0]) : 0;
                        article["title"] = row[1] ? row[1] : "";
                        article["publishTime"] = row[2] ? row[2] : "";
                        article["author"] = row[3] ? row[3] : "Unknown";
                        article["likes"] = row[4];
                        article["views"] = row[5];
                        articles_list.push_back(article);
                    }
                }
            });

            resp.result(boost::beast::http::status::ok);
            resp.body() = json::serialize(articles_list);
        };

        // --- POST ---
        // POST /api/login
        post_routes_["/login"] = [this](auto& req, auto& resp) {
            resp.set(boost::beast::http::field::content_type, "application/json");
            try {
                json::value jv = json::parse(req.body());
                auto& obj = jv.as_object();
                std::string username = obj.at("username").as_string().c_str();
                std::string password = obj.at("password").as_string().c_str();
                bool valid = false;
                long user_id = 0;

                db_stmt_rw("SELECT id FROM user WHERE username = ? AND password = ?", [&](void* arg)
                    {
                        MYSQL_BIND bind = {};
                        bind.buffer_type = MYSQL_TYPE_LONG;
                        bind.buffer = &user_id;

                        if (mysql_stmt_bind_result(static_cast<MYSQL_STMT*>(arg), &bind))
                            return;

                        if (arg && mysql_stmt_fetch(static_cast<MYSQL_STMT*>(arg)) == 0)
                            valid = true;
                    }, username, password);

                //username = escape_sql_string(username);
                //password = escape_sql_string(password);

                //std::string sql = "SELECT id FROM user WHERE username='" + username + "' AND password='" + password + "'";
                //db_query(sql, [&](MYSQL_RES* res) {
                //    MYSQL_ROW row;
                //    if (res && (row = mysql_fetch_row(res))) {
                //        valid = true;
                //        user_id = std::atol(row[0]);
                //    }
                //});

                if (valid) {
                    json::object res_obj;
                    res_obj["token"] = "mock_token_user_" + std::to_string(user_id);
                    resp.result(boost::beast::http::status::ok);
                    resp.body() = json::serialize(res_obj);
                } else {
                    set_json_err(resp, boost::beast::http::status::unauthorized, "Invalid username or password");
                }
            } catch (const std::exception& e) {
                set_json_err(resp, boost::beast::http::status::bad_request, "Invalid JSON format");
            }
        };

        // 发表评论
        // POST /api/comments
        post_routes_["/comments"] = [this](auto& req, auto& resp) {
            resp.set(boost::beast::http::field::content_type, "application/json");
            long user_id = check_auth_and_get_user_id(req, resp);
            if (!user_id) return set_json_err(resp, boost::beast::http::status::unauthorized, "Unauthorized");

            try {
                json::value jv = json::parse(req.body());
                auto& obj = jv.as_object();
                if (!obj.contains("articleId") || !obj.contains("content")) {
                    return set_json_err(resp, boost::beast::http::status::bad_request, "Missing fields");
                }
                int articleId = std::stoi(obj.at("articleId").as_string().c_str());
                std::string content = obj.at("content").as_string().c_str();
                if (content.length() > 8172) {
                    return set_json_err(resp, boost::beast::http::status::bad_request, "Comment content too long");
                }
                //std::string content = escape_sql_string(obj.at("content").as_string().c_str());

                long affected = 0;
                db_stmt_rw("INSERT INTO comment (article_id, user_id, content,created_at) VALUES (?,?,?,NOW())", [&](void* arg)
                    {
                        affected = *(long*)arg;
                    }, articleId, user_id, content);
                //std::string sql = "INSERT INTO comment (article_id, user_id, content,created_at) VALUES (" +
                //    std::to_string(articleId) + ", " + std::to_string(user_id) + ", '" + content + "',NOW())";
                //db_execute(sql, [&](long a) { affected = a; });

                if (affected > 0) {
                    json::object res;
                    res["message"] = "评论成功";
                    res["articleId"] = articleId;
                    resp.result(boost::beast::http::status::created);
                    resp.body() = json::serialize(res);
                } else {
                    set_json_err(resp, boost::beast::http::status::internal_server_error, "Fail to insert comment");
                }
            } catch (...) {
                set_json_err(resp, boost::beast::http::status::bad_request, "Invalid JSON");
            }
        };

        // 创建新资源 (文章)
        // POST /api/articles
        post_routes_["/articles"] = [this](auto& req, auto& resp) {
            resp.set(boost::beast::http::field::content_type, "application/json");
            long user_id = check_auth_and_get_user_id(req, resp);
            if (!user_id) return set_json_err(resp, boost::beast::http::status::unauthorized, "Unauthorized");

            try {
                json::value jv = json::parse(req.body());
                auto& obj = jv.as_object();
                if (!obj.contains("title") || !obj.contains("content")) {
                    return set_json_err(resp, boost::beast::http::status::bad_request, "Missing title or content");
                }

                std::string title = obj.at("title").as_string().c_str();
                std::string content = obj.at("content").as_string().c_str();

                if (title.length() > 255 || content.length() > 8172) {
                    return set_json_err(resp, boost::beast::http::status::bad_request, "Title or content too long");
                }

                //std::string title = escape_sql_string(obj.at("title").as_string().c_str());
                //std::string content = escape_sql_string(obj.at("content").as_string().c_str());

                long affected = 0;
                db_stmt_rw("INSERT INTO article (title, content, user_id,create_time) VALUES (?,?,?,NOW())", [&](void* arg)
                    {
                        affected = *(long*)arg;
                    }, title, content, std::to_string(user_id));

                //std::string sql = "INSERT INTO article (title, content, user_id,create_time) VALUES ('" + title + "', '" + content + "', " + std::to_string(user_id) + ",NOW())";
                //db_execute(sql, [&](long a) { affected = a; });

                if (affected > 0) {
                    resp.result(boost::beast::http::status::created);
                    json::object res;
                    res["message"] = "Create success";
                    resp.body() = json::serialize(res);
                } else {
                    set_json_err(resp, boost::beast::http::status::internal_server_error, "Create failed");
                }
            } catch (...) {
                set_json_err(resp, boost::beast::http::status::bad_request, "Invalid JSON");
            }
        };
    }

    void Router::handle_request(boost::beast::http::request<boost::beast::http::string_body>& req, boost::beast::http::response<boost::beast::http::string_body>& resp) {
        boost::beast::string_view target_beast = req.target();
        std::string_view target(target_beast.data(), target_beast.size());

        std::string_view route_target = target;

        if (route_target.rfind("/api", 0) == 0)
            route_target = route_target.substr(4);
        else
        {
            set_json_err(resp, boost::beast::http::status::not_found, "Wrong url access.");
            return;
        }

        resp.set(boost::beast::http::field::content_type, "application/json");

        // 统一处理参数在URL中的动态路由，即获取参数
        auto extract_id = [](std::string_view route, std::string_view prefix) -> std::string {
            if (route.starts_with(prefix)) {
                return std::string(route.substr(prefix.size()));
            }
            return "";
        };

        switch (req.method()) {
            case boost::beast::http::verb::options:
                resp.result(boost::beast::http::status::no_content);
                break;

            case boost::beast::http::verb::get: {
                //登录与获取文章列表
                if (auto it = get_routes_.find(route_target); it != get_routes_.end()) {
                    it->second(req, resp);
                }
                //获取具体文章
                else if (std::string id_str = extract_id(route_target, "/articles/"); !id_str.empty()) {
                    // GET /api/articles/{id}
                    //id_str = escape_sql_string(id_str);
                    long user_id = check_auth_and_get_user_id(req, resp);
                    bool found = false;
                    json::object res_obj;

                    db_stmt_rw("SELECT a.title,a.content,DATE_FORMAT(a.create_time, '%Y-%m-%d %H:%i:%s') AS create_time,u.username,a.likes,a.views,CASE WHEN ul.id IS NOT NULL THEN 1 ELSE 0 END AS user_liked FROM article a LEFT JOIN user u ON a.user_id = u.id LEFT JOIN user_likes ul ON ul.article_id = a.id AND ul.user_id = ? WHERE a.id = ? ", [&](void* arg)
                        {
                            auto stmt = static_cast<MYSQL_STMT*>(arg);
                            if (!stmt) return;

                            char title_buf[256] = {};
                            std::vector<char> content_buf(2048); // 使用 vector 作为动态缓冲区, 初始大小2048
                            char time_buf[64] = {};
                            char author_buf[256] = {};
                            unsigned long title_len = 0, content_len = 0, time_len = 0, author_len = 0;
                            int likes, views;
                            bool content_is_null = 0, content_error = 0, liked = 0;

                            MYSQL_BIND bind[7] = {};
                            bind[0].buffer_type = MYSQL_TYPE_STRING;
                            bind[0].buffer = title_buf;
                            bind[0].buffer_length = sizeof(title_buf);
                            bind[0].length = &title_len;

                            // 绑定 content 字段到 vector
                            bind[1].buffer_type = MYSQL_TYPE_STRING;
                            bind[1].buffer = content_buf.data();
                            bind[1].buffer_length = content_buf.size();
                            bind[1].length = &content_len;
                            bind[1].is_null = &content_is_null;
                            bind[1].error = &content_error; // 用于接收列的截断状态

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

                            // 获取数据
                            int fetch_result = mysql_stmt_fetch(stmt);

                            // 如果成功或数据被截断，都继续处理
                            if (fetch_result == 0 || fetch_result == MYSQL_DATA_TRUNCATED) {
                                found = true;
                                res_obj["title"] = std::string(title_buf, title_len);
                                res_obj["publishTime"] = std::string(time_buf, time_len);
                                res_obj["author"] = std::string(author_buf, author_len);
                                res_obj["id"] = id_str;
                                res_obj["likes"] = likes;
                                res_obj["views"] = views;
                                res_obj["userLiked"] = liked ? true : false;

                                // 单独处理可能被截断的 content 字段
                                if (!content_is_null) {
                                    std::string content_str;
                                    // 如果 content_error 为 true, 说明 content 字段被截断
                                    if (content_error) {
                                        // content_len 现在是完整数据的长度, 以此重置 vector 大小
                                        content_buf.resize(content_len);
                                        bind[1].buffer = content_buf.data();
                                        bind[1].buffer_length = content_buf.size();

                                        // 使用 mysql_stmt_fetch_column 再次单独获取 content 列的完整数据
                                        if (mysql_stmt_fetch_column(stmt, &bind[1], 1, 0) == 0) {
                                            content_str.assign(content_buf.data(), content_len);
                                        }
                                    }
                                    else {
                                        // 未截断，直接使用
                                        content_str.assign(content_buf.data(), content_len);
                                    }
                                    res_obj["content"] = content_str;
                                }
                                else {
                                    // 如果数据库中是 NULL
                                    res_obj["content"] = "";
                                }
                            }
                        }, user_id, id_str);

                    //std::string sql = "SELECT a.title, a.content, DATE_FORMAT(a.create_time, '%Y-%m-%d %H:%i:%s'), u.username "
                    //                  "FROM article a LEFT JOIN user u ON a.user_id = u.id WHERE a.id=" + id_str;
                    //db_query(sql, [&](MYSQL_RES* res) {
                    //    MYSQL_ROW row;
                    //    if (res && (row = mysql_fetch_row(res))) {
                    //        found = true;
                    //        res_obj["title"] = row[0] ? row[0] : "";
                    //        res_obj["content"] = row[1] ? row[1] : "";
                    //        res_obj["publishTime"] = row[2] ? row[2] : "";
                    //        res_obj["author"] = row[3] ? row[3] : "Unknown";
                    //        res_obj["id"] = id_str;
                    //    }
                    //});
                    if (found) {
                        resp.result(boost::beast::http::status::ok);
                        resp.body() = json::serialize(res_obj);
                    } else {
                        set_json_err(resp, boost::beast::http::status::not_found, "Article not found");
                    }
                }
                //获取具体文章评论
                else if (route_target.starts_with("/comments?articleId=")) {
                    // 获取评论 GET /api/comments?articleId=...
                    std::string id_str = std::string(route_target.substr(20));
                    json::array comments;
                    db_stmt_rw("SELECT c.id, u.username, c.content FROM comment c JOIN user u ON c.user_id = u.id WHERE c.article_id=?",
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
                                json::object c;
                                c["id"] = id;
                                c["author"] = std::string(username_buf, username_len);
                                if (content_len > content_buf.size()) {
                                    content_buf.resize(content_len);
                                    bind[2].buffer = content_buf.data();
                                    bind[2].buffer_length = content_buf.size();
                                    mysql_stmt_fetch_column(stmt, &bind[2], 2, 0);
                                }
                                c["content"] = std::string(content_buf.data(), content_len);
                                comments.push_back(c);
                            }
                        },
                        id_str);
                    resp.result(boost::beast::http::status::ok);
                    resp.body() = json::serialize(comments);
                } else {
                    set_json_err(resp, boost::beast::http::status::not_found, "Not Found");
                }
                break;
            }

            case boost::beast::http::verb::post: {
                // POST /articles/{id}/like (toggle)
                // POST /articles/{id}/view
                if (route_target.rfind("/articles/", 0) == 0) {
                    std::string_view rest = route_target.substr(std::string_view("/articles/").size());
                    auto slash = rest.find('/');
                    if (slash != std::string_view::npos) {
                        // 获取id值
                        std::string_view id_sv = rest.substr(0, slash);
                        // 获取具体行为
                        std::string_view action_sv = rest.substr(slash + 1);

                        if (is_numeric(id_sv) && (action_sv == "like" || action_sv == "view")) {
                            std::string article_id(id_sv);

                            // 点赞逻辑
                            if (action_sv == "like") {
                                long user_id = check_auth_and_get_user_id(req, resp);
                                if (!user_id) {
                                    set_json_err(resp, boost::beast::http::status::unauthorized, "Unauthorized");
                                    break;
                                }

                                // Ensure article exists and get current likes
                                bool article_found = false;
                                long likes_now = 0;
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
                                    set_json_err(resp, boost::beast::http::status::not_found, "Article not found");
                                    break;
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
                                } else {
                                    long affected = 0;
                                    db_stmt_rw("DELETE FROM user_likes WHERE user_id=? AND article_id=?", [&](void* arg) {
                                        affected = *(long*)arg;
                                    }, (int)user_id, article_id);

                                    if (affected > 0) {
                                        db_stmt_rw("UPDATE article SET likes=IF(likes>0, likes-1, 0) WHERE id=?", [](void*) {}, article_id);
                                    }
                                }

                                // Read back current likes and liked status
                                bool liked_after = false;
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

                                if (!article_found) {
                                    set_json_err(resp, boost::beast::http::status::not_found, "Article not found");
                                    break;
                                }

                                resp.result(boost::beast::http::status::ok);
                                json::object out;
                                out["likes"] = likes_now;
                                out["liked"] = liked_after;
                                resp.body() = json::serialize(out);
                                break;
                            }
                            // 浏览量逻辑
                            else if (action_sv == "view") {
                                static std::mutex view_mu;
                                static std::unordered_map<std::string, std::chrono::steady_clock::time_point> view_last;

                                auto now = std::chrono::steady_clock::now();
                                std::string key = get_client_key(req) + "|" + article_id;
                                bool do_inc = true;
                                {
                                    std::scoped_lock lk(view_mu);
                                    auto it = view_last.find(key);
                                    if (it != view_last.end() && (now - it->second) < std::chrono::seconds(10)) {
                                        do_inc = false;
                                    } else {
                                        view_last[key] = now;
                                    }
                                }

                                if (do_inc) {
                                    long affected = 0;
                                    db_stmt_rw("UPDATE article SET views=views+1 WHERE id=?", [&](void* arg) {
                                        affected = *(long*)arg;
                                    }, article_id);
                                    if (affected <= 0) {
                                        set_json_err(resp, boost::beast::http::status::not_found, "Article not found");
                                        break;
                                    }
                                }

                                bool found = false;
                                long views_now = 0;
                                db_stmt_rw("SELECT views FROM article WHERE id=?", [&](void* arg) {
                                    auto stmt = static_cast<MYSQL_STMT*>(arg);
                                    if (!stmt) return;
                                    MYSQL_BIND b = {};
                                    b.buffer_type = MYSQL_TYPE_LONG;
                                    b.buffer = &views_now;
                                    if (mysql_stmt_bind_result(stmt, &b)) return;
                                    if (mysql_stmt_fetch(stmt) == 0) found = true;
                                }, article_id);

                                if (!found) {
                                    set_json_err(resp, boost::beast::http::status::not_found, "Article not found");
                                    break;
                                }

                                resp.result(boost::beast::http::status::ok);
                                json::object out;
                                out["views"] = views_now;
                                resp.body() = json::serialize(out);
                                break;
                            }
                        }
                    }
                }

                if (auto it = post_routes_.find(route_target); it != post_routes_.end()) {
                    it->second(req, resp);
                } else {
                    set_json_err(resp, boost::beast::http::status::not_found, "Not Found");
                }
                break;
            }

            case boost::beast::http::verb::put: {
                if (std::string id_str = extract_id(route_target, "/articles/"); !id_str.empty()) {
                    long user_id = check_auth_and_get_user_id(req, resp);
                    if (!user_id) { set_json_err(resp, boost::beast::http::status::unauthorized, "Unauthorized"); break; }

                    try {
                        json::value jv = json::parse(req.body());
                        auto& obj = jv.as_object();
                        if (!obj.contains("title") || !obj.contains("content")) {
                            set_json_err(resp, boost::beast::http::status::bad_request, "Missing fields");
                            break;
                        }

                        std::string title = obj.at("title").as_string().c_str();
                        std::string content = obj.at("content").as_string().c_str();

                        if (title.length() > 255 || content.length() > 8172) {
                            set_json_err(resp, boost::beast::http::status::bad_request, "Title or content too long");
                            break;
                        }

                        long affected = 0;
                        db_stmt_rw("UPDATE article SET title=?, content=? WHERE id=? AND user_id=?", 
                            [&](void* arg) { affected = *(long*)arg; }, 
                            title, content, id_str, user_id);

                        if (affected > 0) {
                            resp.result(boost::beast::http::status::ok);
                            json::object res; res["message"] = "Updated";
                            resp.body() = json::serialize(res);
                        } else {
                            set_json_err(resp, boost::beast::http::status::forbidden, "Not found or not your article");
                        }
                    } catch (...) {
                        set_json_err(resp, boost::beast::http::status::bad_request, "Invalid JSON");
                    }
                } else {
                    set_json_err(resp, boost::beast::http::status::not_found, "Not Found");
                }
                break;
            }

            case boost::beast::http::verb::patch: {
                if (std::string id_str = extract_id(route_target, "/articles/"); !id_str.empty()) {
                    long user_id = check_auth_and_get_user_id(req, resp);
                    if (!user_id) { set_json_err(resp, boost::beast::http::status::unauthorized, "Unauthorized"); break; }

                    try {
                        json::value jv = json::parse(req.body());
                        auto& obj = jv.as_object();

                        bool has_title = obj.contains("title");
                        bool has_content = obj.contains("content");

                        if (!has_title && !has_content) {
                            set_json_err(resp, boost::beast::http::status::bad_request, "Nothing to patch");
                            break;
                        }

                        std::string title = has_title ? obj.at("title").as_string().c_str() : "";
                        std::string content = has_content ? obj.at("content").as_string().c_str() : "";

                        if (has_title && title.length() > 255) {
                            set_json_err(resp, boost::beast::http::status::bad_request, "Title too long");
                            break;
                        }
                        if (has_content && content.length() > 8172) {
                            set_json_err(resp, boost::beast::http::status::bad_request, "Content too long");
                            break;
                        }

                        long affected = 0;
                        if (has_title && has_content) {
                            db_stmt_rw("UPDATE article SET title=?, content=? WHERE id=? AND user_id=?", 
                                [&](void* arg) { affected = *(long*)arg; }, 
                                title, content, id_str, user_id);
                        } else if (has_title) {
                            db_stmt_rw("UPDATE article SET title=? WHERE id=? AND user_id=?", 
                                [&](void* arg) { affected = *(long*)arg; }, 
                                title, id_str, user_id);
                        } else if (has_content) {
                            db_stmt_rw("UPDATE article SET content=? WHERE id=? AND user_id=?", 
                                [&](void* arg) { affected = *(long*)arg; }, 
                                content, id_str, user_id);
                        }

                        if (affected > 0) {
                            resp.result(boost::beast::http::status::ok);
                            json::object res; res["message"] = "Patched";
                            resp.body() = json::serialize(res);
                        } else {
                            set_json_err(resp, boost::beast::http::status::forbidden, "Not found or not your article");
                        }
                    } catch (...) {
                        set_json_err(resp, boost::beast::http::status::bad_request, "Invalid JSON");
                    }
                } else {
                    set_json_err(resp, boost::beast::http::status::not_found, "Not Found");
                }
                break;
            }
            

            case boost::beast::http::verb::delete_: {
                if (std::string id_str = extract_id(route_target, "/articles/"); !id_str.empty()) {
                    long user_id = check_auth_and_get_user_id(req, resp);
                    if (!user_id) { set_json_err(resp, boost::beast::http::status::unauthorized, "Unauthorized"); break; }

                    long affected = 0;
                    db_stmt_rw("DELETE FROM article WHERE id=? AND user_id=?", 
                        [&](void* arg) { affected = *(long*)arg; }, 
                        id_str, user_id);

                    if (affected > 0) {
                        resp.result(boost::beast::http::status::ok);
                        json::object res; res["message"] = "Deleted";
                        resp.body() = json::serialize(res);
                    } else {
                        set_json_err(resp, boost::beast::http::status::forbidden, "Not found or not your article");
                    }
                }
                else if (std::string id_str = extract_id(route_target, "/comments/"); !id_str.empty()) {
                    long user_id = check_auth_and_get_user_id(req, resp);
                    if (!user_id) { set_json_err(resp, boost::beast::http::status::unauthorized, "Unauthorized"); break; }

                    long affected = 0;
                    db_stmt_rw("DELETE FROM comment WHERE id=? AND user_id=?", 
                        [&](void* arg) { affected = *(long*)arg; }, 
                        id_str, user_id);

                    if (affected > 0) {
                        resp.result(boost::beast::http::status::ok);
                        json::object res; res["message"] = "Deleted";
                        resp.body() = json::serialize(res);
                    }
                    else {
                        set_json_err(resp, boost::beast::http::status::forbidden, "Not found or not your comment");
                    }
                }
                else {
                    set_json_err(resp, boost::beast::http::status::not_found, "Not Found");
                }
                break;
            }

            default:
                set_json_err(resp, boost::beast::http::status::method_not_allowed, "Method Not Allowed");
                break;
        };
    }

    bool Router::db_query(const std::string& sql, std::function<void(MYSQL_RES*)>&& callback) noexcept
    {
        return ConnPool::getInstance().query(sql, std::move(callback));
    }

    //bool Router::db_execute(const std::string& sql, std::function<void(long)>&& callback) noexcept
    //{
    //    return ConnPool::getInstance().execute(sql, std::move(callback));
    //}

    //std::string Router::escape_sql_string(const std::string& input) {
    //    return ConnPool::getInstance().escapeString(input);
    //}
}
