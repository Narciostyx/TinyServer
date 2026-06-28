#include "router.hpp"
#include <sstream>
#include <boost/json.hpp>
#include <regex>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <cctype>
#include <ctime>
#include <jwt.h>

#include "metrics.hpp"

namespace json = boost::json;

namespace project {
    Router::Router() : Router(Config{}) {}

    Router::Router(const Config& cfg) : config_(cfg) {
        jwt_secret_ = config_.jwt_secret;
        jwt_access_exp_seconds_ = config_.jwt_access_exp_seconds;
        jwt_refresh_exp_seconds_ = config_.jwt_refresh_exp_seconds;
        init_routes();
    }

    //认证请求体
    struct AuthContext {
        long user_id = 0;
        std::string role;
        bool is_refresh = false;
    };

    //获取token值
    static bool extract_token(const boost::beast::http::request<boost::beast::http::string_body>& req, std::string& out) {
        auto auth_it = req.find(boost::beast::http::field::authorization);
        if (auth_it == req.end()) {
            return false;
        }
        auto val = auth_it->value();
        if (!val.starts_with("Bearer ")) {
            return false;
        }
        out.assign(val.substr(7).data(), val.substr(7).size());
        return !out.empty();
    }

    //授权认证与获取认证体内容
    static bool check_auth_and_get_context(const boost::beast::http::request<boost::beast::http::string_body>& req,
                                           const std::string& secret,
                                           AuthContext& ctx,
                                           bool allow_refresh = false,
                                           std::string* err = nullptr) {
        std::string token;
        if (!extract_token(req, token)) {
            if (err) *err = "Missing token";
            return false;
        }

        jwt_t* jwt = nullptr;
        if (jwt_decode(&jwt, token.c_str(), (const unsigned char*)secret.data(), secret.size())) {
            if (err) *err = "Invalid token";
            return false;
        }

        auto type = jwt_get_grant(jwt, "type");
        ctx.is_refresh = type && std::string(type) == "refresh";

        long long exp = jwt_get_grant_int(jwt, "exp");
        if (exp > 0 && std::time(nullptr) >= exp) {
            jwt_free(jwt);
            if (err) *err = "Token expired";
            return false;
        }

        if (ctx.is_refresh && !allow_refresh) {
            jwt_free(jwt);
            if (err) *err = "Invalid token type";
            return false;
        }

        auto role = jwt_get_grant(jwt, "role");
        if (role) {
            ctx.role = role;
        }

        long uid = 0;
        auto user_id = jwt_get_grant(jwt, "user_id");
        if (user_id) {
            try {
                uid = std::stol(user_id);
            }
            catch (...) {
                uid = 0;
            }
        }

        ctx.user_id = uid;
        jwt_free(jwt);
        if (ctx.user_id <= 0) {
            if (err) *err = "Invalid user";
            return false;
        }
        return true;
    }

    //生成jwt令牌
    static std::string build_jwt_token(const std::string& secret, long exp_seconds, long user_id, const std::string& role, const std::string& type) {
        jwt_t* jwt = nullptr;
        if (jwt_new(&jwt) != 0) {
            return {};
        }
        jwt_add_grant_int(jwt, "user_id", user_id);
        jwt_add_grant(jwt, "role", role.c_str());
        jwt_add_grant(jwt, "type", type.c_str());
        jwt_add_grant_int(jwt, "exp", static_cast<long>(std::time(nullptr) + exp_seconds));
        jwt_set_alg(jwt, JWT_ALG_HS256, (const unsigned char*)secret.data(), secret.size());

        char* out = jwt_encode_str(jwt);
        std::string token = out ? out : "";
        if (out) {
            jwt_free_str(out);
        }
        jwt_free(jwt);
        return token;
    }

    // 返回错误信息json
    static void set_json_err(auto& resp, boost::beast::http::status status, const std::string& msg) {
        resp.result(status);
        json::object err_obj;
        err_obj["message"] = msg;
        resp.body() = json::serialize(err_obj);
    }

    //辅助校验函数
    static bool is_numeric(std::string_view s) {
        if (s.empty()) return false;
        for (unsigned char ch : s) {
            if (!std::isdigit(ch)) return false;
        }
        return true;
    }

    //获取用户内容
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

            service_.fetch_articles(articles_list);

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
                bool valid = false, admin = false;
                long user_id = 0;
                service_.authenticate_user(username, password, user_id, admin, valid);

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
                    std::string role = admin ? "admin" : "user";
                    std::string access_token = build_jwt_token(jwt_secret_, jwt_access_exp_seconds_, user_id, role, "access");
                    std::string refresh_token = build_jwt_token(jwt_secret_, jwt_refresh_exp_seconds_, user_id, role, "refresh");
                    res_obj["accessToken"] = access_token;
                    res_obj["refreshToken"] = refresh_token;
                    res_obj["role"] = role;
                    res_obj["expiresIn"] = jwt_access_exp_seconds_;
                    resp.result(boost::beast::http::status::ok);
                    resp.body() = json::serialize(res_obj);
                } else {
                    set_json_err(resp, boost::beast::http::status::unauthorized, "Invalid username or password");
                }
            } catch (const std::exception& e) {
                set_json_err(resp, boost::beast::http::status::bad_request, "Invalid JSON format");
            }
        };

        // POST /api/refresh
        post_routes_["/refresh"] = [this](auto& req, auto& resp) {
            resp.set(boost::beast::http::field::content_type, "application/json");
            AuthContext auth;
            if (!check_auth_and_get_context(req, jwt_secret_, auth) || !auth.is_refresh) {
                return set_json_err(resp, boost::beast::http::status::unauthorized, "Unauthorized");
            }

            std::string access_token = build_jwt_token(jwt_secret_, jwt_access_exp_seconds_, auth.user_id, auth.role, "access");
            json::object res_obj;
            res_obj["accessToken"] = access_token;
            res_obj["expiresIn"] = jwt_access_exp_seconds_;
            resp.result(boost::beast::http::status::ok);
            resp.body() = json::serialize(res_obj);
        };

        // 发表评论
        // POST /api/comments
        post_routes_["/comments"] = [this](auto& req, auto& resp) {
            resp.set(boost::beast::http::field::content_type, "application/json");
            AuthContext auth;
            if (!check_auth_and_get_context(req, jwt_secret_, auth, false)) {
                return set_json_err(resp, boost::beast::http::status::unauthorized, "Unauthorized");
            }
            long user_id = auth.user_id;

            try {
                json::value jv = json::parse(req.body());
                auto& obj = jv.as_object();
                if (!obj.contains("articleId") || !obj.contains("content")) {
                    return set_json_err(resp, boost::beast::http::status::bad_request, "Missing fields");
                }
                int articleId = std::stoi(obj.at("articleId").as_string().c_str());
                std::string content = obj.at("content").as_string().c_str();
                if (content.length() > kContentLength) {
                    return set_json_err(resp, boost::beast::http::status::bad_request, "Comment content too long");
                }
                //std::string content = escape_sql_string(obj.at("content").as_string().c_str());

                long affected = 0;
                service_.create_comment(articleId, user_id, content, affected);
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
            AuthContext auth;
            if (!check_auth_and_get_context(req, jwt_secret_, auth, false)) {
                return set_json_err(resp, boost::beast::http::status::unauthorized, "Unauthorized");
            }
            long user_id = auth.user_id;

            try {
                json::value jv = json::parse(req.body());
                auto& obj = jv.as_object();
                if (!obj.contains("title") || !obj.contains("content")) {
                    return set_json_err(resp, boost::beast::http::status::bad_request, "Missing title or content");
                }

                std::string title = obj.at("title").as_string().c_str();
                std::string content = obj.at("content").as_string().c_str();

                if (title.length() > kTitleLength || content.length() > kContentLength) {
                    return set_json_err(resp, boost::beast::http::status::bad_request, "Title or content too long");
                }

                //std::string title = escape_sql_string(obj.at("title").as_string().c_str());
                //std::string content = escape_sql_string(obj.at("content").as_string().c_str());

                long affected = 0;
                service_.create_article(title, content, user_id, affected);

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

        auto is_exact_or_query = [](std::string_view path, std::string_view expected) {
            return path == expected ||
                (path.size() > expected.size() && path.starts_with(expected) && path[expected.size()] == '?');
        };

        if (req.method() == boost::beast::http::verb::get) {
            if (is_exact_or_query(target, "/metrics")) {
                resp.result(boost::beast::http::status::ok);
                resp.set(boost::beast::http::field::content_type, "text/plain; version=0.0.4");
                resp.body() = MetricsRegistry::instance().render_prometheus();
                return;
            }

            if (is_exact_or_query(target, "/healthz/live") || is_exact_or_query(target, "/livez")) {
                bool up = MetricsRegistry::instance().is_live();
                resp.result(up ? boost::beast::http::status::ok : boost::beast::http::status::service_unavailable);
                resp.set(boost::beast::http::field::content_type, "application/json");
                resp.body() = MetricsRegistry::instance().render_health_json(false);
                return;
            }

            if (is_exact_or_query(target, "/healthz/ready") || is_exact_or_query(target, "/readyz")) {
                bool up = MetricsRegistry::instance().is_ready();
                resp.result(up ? boost::beast::http::status::ok : boost::beast::http::status::service_unavailable);
                resp.set(boost::beast::http::field::content_type, "application/json");
                resp.body() = MetricsRegistry::instance().render_health_json(true);
                return;
            }
        }

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
                    long user_id = 0;
                    AuthContext auth;
                    if (check_auth_and_get_context(req, jwt_secret_, auth, false)) {
                        user_id = auth.user_id;
                    }
                    bool found = false;
                    json::object res_obj;
                    service_.fetch_article_detail(id_str, user_id, res_obj, found);

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
                    }
                    else {
                        set_json_err(resp, boost::beast::http::status::not_found, "Article not found");
                    }
                }
                //获取具体文章评论
                else if (route_target.starts_with("/comments?articleId=")) {
                    // 获取评论 GET /api/comments?articleId=...
                    std::string id_str = std::string(route_target.substr(20));
                    json::array comments;
                    service_.fetch_comments(id_str, comments);
                    resp.result(boost::beast::http::status::ok);
                    resp.body() = json::serialize(comments);
                }
                else if (route_target.starts_with("/user/stats"))
                {
                    AuthContext auth;
                    if (!check_auth_and_get_context(req, jwt_secret_, auth, false)) {
                        set_json_err(resp, boost::beast::http::status::unauthorized, "Unauthorized");
                        break;
                    }
                    json::object res_obj;
                    service_.fetch_user_stats(auth.user_id, res_obj);
                    resp.result(boost::beast::http::status::ok);
                    resp.body() = json::serialize(res_obj);
                }
                else {
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
                                AuthContext auth;
                                if (!check_auth_and_get_context(req, jwt_secret_, auth, false)) {
                                    set_json_err(resp, boost::beast::http::status::unauthorized, "Unauthorized");
                                    break;
                                }
                                long user_id = auth.user_id;

                                bool article_found = false;
                                long likes_now = 0;
                                bool liked_after = false;
                                service_.toggle_like(article_id, user_id, likes_now, liked_after, article_found);

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

                                bool found = false;
                                long views_now = 0;
                                service_.update_view(article_id, do_inc, views_now, found);

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
                    AuthContext auth;
                    if (!check_auth_and_get_context(req, jwt_secret_, auth, false)) { set_json_err(resp, boost::beast::http::status::unauthorized, "Unauthorized"); break; }
                    long user_id = auth.user_id;

                    try {
                        json::value jv = json::parse(req.body());
                        auto& obj = jv.as_object();
                        if (!obj.contains("title") || !obj.contains("content")) {
                            set_json_err(resp, boost::beast::http::status::bad_request, "Missing fields");
                            break;
                        }

                        std::string title = obj.at("title").as_string().c_str();
                        std::string content = obj.at("content").as_string().c_str();

                        if (title.length() > kTitleLength || content.length() > kContentLength) {
                            set_json_err(resp, boost::beast::http::status::bad_request, "Title or content too long");
                            break;
                        }

                        long affected = 0;
                        service_.update_article(id_str, user_id, title, content, affected);

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
                    AuthContext auth;
                    if (!check_auth_and_get_context(req, jwt_secret_, auth, false)) { set_json_err(resp, boost::beast::http::status::unauthorized, "Unauthorized"); break; }
                    long user_id = auth.user_id;

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

                        if (has_title && title.length() > kTitleLength) {
                            set_json_err(resp, boost::beast::http::status::bad_request, "Title too long");
                            break;
                        }
                        if (has_content && content.length() > kContentLength) {
                            set_json_err(resp, boost::beast::http::status::bad_request, "Content too long");
                            break;
                        }

                        long affected = 0;
                        if (has_title && has_content) {
                            service_.update_article(id_str, user_id, title, content, affected);
                        } else if (has_title) {
                            service_.update_article_title(id_str, user_id, title, affected);
                        } else if (has_content) {
                            service_.update_article_content(id_str, user_id, content, affected);
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
                    AuthContext auth;
                    if (!check_auth_and_get_context(req, jwt_secret_, auth, false)) { set_json_err(resp, boost::beast::http::status::unauthorized, "Unauthorized"); break; }
                    long user_id = auth.user_id;

                    long affected = 0;
                    service_.delete_article(id_str, user_id, affected);

                    if (affected > 0) {
                        resp.result(boost::beast::http::status::ok);
                        json::object res; res["message"] = "Deleted";
                        resp.body() = json::serialize(res);
                    } else {
                        set_json_err(resp, boost::beast::http::status::forbidden, "Not found or not your article");
                    }
                }
                else if (std::string id_str = extract_id(route_target, "/comments/"); !id_str.empty()) {
                    AuthContext auth;
                    if (!check_auth_and_get_context(req, jwt_secret_, auth, false)) { set_json_err(resp, boost::beast::http::status::unauthorized, "Unauthorized"); break; }
                    long user_id = auth.user_id;

                    long affected = 0;
                    service_.delete_comment(id_str, user_id, affected);

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

    //std::string Router::escape_sql_string(const std::string& input) {
    //    return ConnPool::getInstance().escapeString(input);
    //}
}
