#include "router.hpp"
#include <sstream>
#include "connectionpool.hpp"
#include <boost/json.hpp>
#include <regex>

namespace json = boost::json;

namespace project {
    Router::Router() {
        init_routes();
    }

    // Helper checking token and returning user_id (0 on failure)
    long check_auth_and_get_user_id(auto& req, auto& resp) {
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

    // Helper returning common json err
    void set_json_err(auto& resp, boost::beast::http::status status, const std::string& msg) {
        resp.result(status);
        json::object err_obj;
        err_obj["message"] = msg;
        resp.body() = json::serialize(err_obj);
    }

    void Router::init_routes() {
        // --- GET ---
        get_routes_["/ping"] = [](auto& req, auto& resp) {
            resp.result(boost::beast::http::status::ok);
            resp.body() = "pong";
        };

        // 获取文章列表
        get_routes_["/articles"] = [this](auto& req, auto& resp) {
            resp.set(boost::beast::http::field::content_type, "application/json");
            json::array articles_list;

            std::string sql = "SELECT a.id, a.title, DATE_FORMAT(a.create_time, '%Y-%m-%d %H:%i:%s'), u.username "
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
                        articles_list.push_back(article);
                    }
                }
            });

            resp.result(boost::beast::http::status::ok);
            resp.body() = json::serialize(articles_list);
        };

        // --- POST ---
        post_routes_["/login"] = [this](auto& req, auto& resp) {
            resp.set(boost::beast::http::field::content_type, "application/json");
            try {
                json::value jv = json::parse(req.body());
                auto& obj = jv.as_object();
                std::string username = obj.at("username").as_string().c_str();
                std::string password = obj.at("password").as_string().c_str();

                username = escape_sql_string(username);
                password = escape_sql_string(password);

                bool valid = false;
                long user_id = 0;

                std::string sql = "SELECT id FROM user WHERE username='" + username + "' AND password='" + password + "'";
                db_query(sql, [&](MYSQL_RES* res) {
                    MYSQL_ROW row;
                    if (res && (row = mysql_fetch_row(res))) {
                        valid = true;
                        user_id = std::atol(row[0]);
                    }
                });

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
                std::string content = escape_sql_string(obj.at("content").as_string().c_str());

                long affected = 0;
                std::string sql = "INSERT INTO comment (article_id, user_id, content,created_at) VALUES (" +
                    std::to_string(articleId) + ", " + std::to_string(user_id) + ", '" + content + "',NOW())";
                db_execute(sql, [&](long a) { affected = a; });

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

                std::string title = escape_sql_string(obj.at("title").as_string().c_str());
                std::string content = escape_sql_string(obj.at("content").as_string().c_str());

                long affected = 0;
                std::string sql = "INSERT INTO article (title, content, user_id,create_time) VALUES ('" + title + "', '" + content + "', " + std::to_string(user_id) + ",NOW())";
                db_execute(sql, [&](long a) { affected = a; });

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
        if (route_target.rfind("/api", 0) == 0) {
            route_target = route_target.substr(4);
        }

        resp.set(boost::beast::http::field::content_type, "application/json");

        // 统一处理参数在URL中的动态路由
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
                if (auto it = get_routes_.find(route_target); it != get_routes_.end()) {
                    it->second(req, resp);
                } else if (std::string id_str = extract_id(route_target, "/articles/"); !id_str.empty()) {
                    // GET /api/articles/{id}
                    id_str = escape_sql_string(id_str);
                    bool found = false;
                    json::object res_obj;
                    std::string sql = "SELECT a.title, a.content, DATE_FORMAT(a.create_time, '%Y-%m-%d %H:%i:%s'), u.username "
                                      "FROM article a LEFT JOIN user u ON a.user_id = u.id WHERE a.id=" + id_str;
                    db_query(sql, [&](MYSQL_RES* res) {
                        MYSQL_ROW row;
                        if (res && (row = mysql_fetch_row(res))) {
                            found = true;
                            res_obj["title"] = row[0] ? row[0] : "";
                            res_obj["content"] = row[1] ? row[1] : "";
                            res_obj["publishTime"] = row[2] ? row[2] : "";
                            res_obj["author"] = row[3] ? row[3] : "Unknown";
                            res_obj["id"] = id_str;
                        }
                    });
                    if (found) {
                        resp.result(boost::beast::http::status::ok);
                        resp.body() = json::serialize(res_obj);
                    } else {
                        set_json_err(resp, boost::beast::http::status::not_found, "Article not found");
                    }
                } else if (route_target.starts_with("/comments?articleId=")) {
                    // 获取评论 GET /api/comments?articleId=...
                    std::string id_str = escape_sql_string(std::string(route_target.substr(20)));
                    json::array comments;
                    std::string sql = "SELECT u.username, c.content FROM comment c JOIN user u ON c.user_id = u.id WHERE c.article_id=" + id_str;
                    db_query(sql, [&](MYSQL_RES* res) {
                        if (res) {
                            MYSQL_ROW row;
                            while ((row = mysql_fetch_row(res))) {
                                json::object c;
                                c["author"] = row[0] ? row[0] : "Anonymous";
                                c["content"] = row[1] ? row[1] : "";
                                comments.push_back(c);
                            }
                        }
                    });
                    resp.result(boost::beast::http::status::ok);
                    resp.body() = json::serialize(comments);
                } else {
                    set_json_err(resp, boost::beast::http::status::not_found, "Not Found");
                }
                break;
            }

            case boost::beast::http::verb::post: {
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
                    if (!user_id) break; // check_auth_and_get_user_id return with err via resp missing here in single line, use logic below:
                    
                    try {
                        json::value jv = json::parse(req.body());
                        auto& obj = jv.as_object();
                        if (!obj.contains("title") || !obj.contains("content")) {
                            set_json_err(resp, boost::beast::http::status::bad_request, "Missing fields");
                            break;
                        }
                        
                        id_str = escape_sql_string(id_str);
                        std::string title = escape_sql_string(obj.at("title").as_string().c_str());
                        std::string content = escape_sql_string(obj.at("content").as_string().c_str());

                        long affected = 0;
                        // For put we update everything
                        std::string sql = "UPDATE article SET title='" + title + "', content='" + content + "' WHERE id=" + id_str + " AND user_id=" + std::to_string(user_id);
                        db_execute(sql, [&](long a) { affected = a; });

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
                        
                        id_str = escape_sql_string(id_str);
                        std::string updates;
                        
                        if (obj.contains("title")) { // Optional branch
                            updates += "title='" + escape_sql_string(obj.at("title").as_string().c_str()) + "', ";
                        }
                        if (obj.contains("content")) { // Optional branch
                            updates += "content='" + escape_sql_string(obj.at("content").as_string().c_str()) + "', ";
                        }

                        if (updates.empty()) {
                            set_json_err(resp, boost::beast::http::status::bad_request, "Nothing to patch");
                            break;
                        }
                        
                        updates.pop_back(); updates.pop_back(); // Remove trailing comma and space

                        long affected = 0;
                        std::string sql = "UPDATE article SET " + updates + " WHERE id=" + id_str + " AND user_id=" + std::to_string(user_id);
                        db_execute(sql, [&](long a) { affected = a; });

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
                    
                    id_str = escape_sql_string(id_str);
                    long affected = 0;
                    std::string sql = "DELETE FROM article WHERE id=" + id_str + " AND user_id=" + std::to_string(user_id);
                    db_execute(sql, [&](long a) { affected = a; });

                    if (affected > 0) {
                        resp.result(boost::beast::http::status::ok);
                        json::object res; res["message"] = "Deleted";
                        resp.body() = json::serialize(res);
                    } else {
                        set_json_err(resp, boost::beast::http::status::forbidden, "Not found or not your article");
                    }
                } else {
                    set_json_err(resp, boost::beast::http::status::not_found, "Not Found");
                }
                break;
            }

            default:
                set_json_err(resp, boost::beast::http::status::method_not_allowed, "Method Not Allowed");
                break;
        }
    }

    bool Router::db_query(const std::string& sql, std::function<void(MYSQL_RES*)>&& callback) noexcept
    {
        return ConnPool::getInstance().query(sql, std::move(callback));
    }

    bool Router::db_execute(const std::string& sql, std::function<void(long)>&& callback) noexcept
    {
        return ConnPool::getInstance().execute(sql, std::move(callback));
    }

    std::string Router::escape_sql_string(const std::string& input) {
        return ConnPool::getInstance().escapeString(input);
    }
}
