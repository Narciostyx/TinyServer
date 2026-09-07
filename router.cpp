#include "router.hpp"
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <sstream>
#include <boost/json.hpp>
#include <regex>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <string>
#include <chrono>
#include <cctype>
#include <ctime>
#include <jwt.h>

#include "metrics.hpp"
#include "redis_store.hpp"

namespace json = boost::json;

namespace project {

    // 浏览量去重：分片锁 + 容量上限 + 过期清理
    // 修复原实现（进程内单把全局锁 + 永不过期 map）的锁热点与内存无限增长问题。
    // 注意：进程内状态在多进程部署（SO_REUSEPORT 多 acceptor）下仍会失效，生产应改用 Redis（SETEX + INCR）。
    class ViewDedup {
    public:
        static constexpr std::chrono::seconds kWindow{ 10 };
        static constexpr size_t kShards = 16;
        static constexpr size_t kMaxPerShard = 4096;

        // 返回 true 表示本次浏览应当计数（窗口内未重复）；返回 false 表示去重
        bool should_count(const std::string& key, std::chrono::steady_clock::time_point now) {
            const size_t shard = std::hash<std::string>{}(key) % kShards;
            std::scoped_lock lk(mu_[shard]);
            auto& m = map_[shard];
            auto it = m.find(key);
            if (it != m.end() && now - it->second < kWindow)
                return false;
            m[key] = now;
            // 超过容量阈值时清理过期项，防止内存无限增长
            if (m.size() >= kMaxPerShard) {
                for (auto iter = m.begin(); iter != m.end();) {
                    if (now - iter->second >= kWindow)
                        iter = m.erase(iter);
                    else
                        ++iter;
                }
            }
            return true;
        }

    private:
        std::mutex mu_[kShards];
        std::unordered_map<std::string, std::chrono::steady_clock::time_point> map_[kShards];
    };

    static ViewDedup g_view_dedup;

    // token 黑名单 key：对原始 JWT 做 FNV-1a 64 位哈希再转 hex，
    // 避免把数百字节的 JWT 直接作为 Redis key
    static std::string token_blacklist_key(const std::string& token) {
        std::uint64_t h = 1469598103934665603ULL;
        for (unsigned char c : token) {
            h ^= c;
            h *= 1099511628211ULL;
        }
        char buf[20];
        std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
        return std::string("token_bl:") + buf;
    }

    // 登录失败限流：同一用户名连续失败 5 次，300 秒内拒绝再次尝试（防爆破）
    static constexpr long kLoginFailThreshold = 5;
    static constexpr long kLoginFailWindowSec = 300;

    // 逐 UTF-8 码点校验"仅允许 ASCII 字母/数字/中文"的字符集，并统计字符数、是否含字母/数字。
    // 中文字符按 1 个字符计（中文一字 = 英文一个字符）；非法 UTF-8 或含其它字符返回 false。
    static bool scan_allowed_chars(const std::string& s, int& count, bool& has_letter, bool& has_digit)
    {
        count = 0;
        has_letter = false;
        has_digit = false;
        std::size_t i = 0;
        const std::size_t n = s.size();
        while (i < n)
        {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            int cp = 0;
            int seq = 1;
            if (c < 0x80) { cp = c; seq = 1; }
            else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; seq = 2; }
            else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; seq = 3; }
            else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; seq = 4; }
            else return false; // 非法 UTF-8 首字节
            if (i + static_cast<std::size_t>(seq) > n) return false;
            for (int k = 1; k < seq; ++k)
            {
                const unsigned char cc = static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]);
                if ((cc & 0xC0) != 0x80) return false; // 非法后续字节
                cp = (cp << 6) | (cc & 0x3F);
            }
            i += static_cast<std::size_t>(seq);

            if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z'))
                has_letter = true;
            else if (cp >= '0' && cp <= '9')
                has_digit = true;
            else if (!(cp >= 0x4E00 && cp <= 0x9FFF) && !(cp >= 0x3400 && cp <= 0x4DBF))
                return false; // 既非 ASCII 字母数字、也非 CJK（含 CJK 扩展A）→ 特殊字符，拒绝
            ++count;
        }
        return true;
    }

    // 统计 UTF-8 字符串的字符数（中文一字 = 1 字符）。
    // 用于对标题/正文做"按字符"的长度限制（数据库 varchar/text 列也按字符计数），
    // 避免用字节数误拒中文内容（1 个汉字在 UTF-8 下占 3 字节）。
    static std::size_t utf8_char_count(const std::string& s) {
        std::size_t count = 0;
        std::size_t i = 0;
        const std::size_t n = s.size();
        while (i < n)
        {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            if (c < 0x80) { ++count; i += 1; }
            else if ((c & 0xE0) == 0xC0) { ++count; i += 2; }
            else if ((c & 0xF0) == 0xE0) { ++count; i += 3; }
            else if ((c & 0xF8) == 0xF0) { ++count; i += 4; }
            else { ++count; i += 1; } // 非法字节按 1 个字符计，宽松处理
        }
        return count;
    }

    Router::Router() : Router(Config{}) {}

    Router::Router(const Config& cfg) {
        apply_config(cfg);
        init_routes();
    }

    void Router::apply_config(const Config& cfg) {
        config_ = cfg;
        jwt_secret_ = cfg.jwt_secret;
        jwt_access_exp_seconds_ = cfg.jwt_access_exp_seconds;
        jwt_refresh_exp_seconds_ = cfg.jwt_refresh_exp_seconds;
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

        // 防御：服务端未配置 JWT 密钥时直接拒绝，避免空密钥传入 libjwt 引发未定义行为
        if (secret.empty()) {
            if (err) *err = "Server JWT secret not configured";
            return false;
        }

        jwt_t* jwt = nullptr;
        if (jwt_decode(&jwt, token.c_str(), (const unsigned char*)secret.data(), (int)secret.size())) {
            LOG_DEBUG(std::string("jwt_decode failed, token head: ") + token.substr(0, 40));
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

        // 注意：user_id 在签发时用 jwt_add_grant_int（数值）写入，
        // 必须用 jwt_get_grant_int 读取——jwt_get_grant() 只返回字符串型 grant，对数值型返回 NULL
        long uid = static_cast<long>(jwt_get_grant_int(jwt, "user_id"));

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
        if (secret.empty()) {
            LOG_ERR("JWT secret is empty, refuse to build token.");
            return {};
        }
        jwt_t* jwt = nullptr;
        if (jwt_new(&jwt) != 0) {
            return {};
        }
        jwt_add_grant_int(jwt, "user_id", user_id);
        jwt_add_grant(jwt, "role", role.c_str());
        jwt_add_grant(jwt, "type", type.c_str());
        jwt_add_grant_int(jwt, "exp", static_cast<long>(std::time(nullptr) + exp_seconds));
        // 必须校验返回值：若 set_alg 失败，encode 会签发 alg=none 的无签名 token，
        // 之后所有需鉴权接口都会解码失败（表现为"上传不了/认证不过"）
        if (jwt_set_alg(jwt, JWT_ALG_HS256, (const unsigned char*)secret.data(), (int)secret.size()) != 0) {
            LOG_ERR("jwt_set_alg(JWT_ALG_HS256) failed, refuse to sign token. Check libjwt backend (openssl/gnutls).");
            jwt_free(jwt);
            return {};
        }

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
        // POST /api/register —— 用户注册
        // 规则：用户名 1~8 个字符（中文一字按 1 字符计）；密码 8~12 位且必须同时含字母与数字；
        //       用户名与密码均仅允许 ASCII 字母、数字、中文字符（不含任何特殊字符）。
        post_routes_["/register"] = [this](auto& req, auto& resp) {
            resp.set(boost::beast::http::field::content_type, "application/json");
            try {
                json::value jv = json::parse(req.body());
                auto& obj = jv.as_object();
                if (!obj.contains("username") || !obj.contains("password")) {
                    return set_json_err(resp, boost::beast::http::status::bad_request, "Missing username or password");
                }

                std::string username = obj.at("username").as_string().c_str();
                std::string password = obj.at("password").as_string().c_str();

                // 用户名：1~8 字符，仅字母/数字/中文
                int u_count = 0;
                bool u_letter = false, u_digit = false;
                if (!scan_allowed_chars(username, u_count, u_letter, u_digit)) {
                    return set_json_err(resp, boost::beast::http::status::bad_request, "用户名只能包含字母、数字或中文字符");
                }
                if (u_count < 1 || u_count > 8) {
                    return set_json_err(resp, boost::beast::http::status::bad_request, "用户名长度需为 1~8 个字符");
                }

                // 密码：8~12 位，仅字母/数字/中文，且必须同时含字母与数字
                int p_count = 0;
                bool p_letter = false, p_digit = false;
                if (!scan_allowed_chars(password, p_count, p_letter, p_digit)) {
                    return set_json_err(resp, boost::beast::http::status::bad_request, "密码只能包含字母、数字或中文字符");
                }
                if (p_count < 8 || p_count > 12) {
                    return set_json_err(resp, boost::beast::http::status::bad_request, "密码长度需为 8~12 位");
                }
                if (!p_letter || !p_digit) {
                    return set_json_err(resp, boost::beast::http::status::bad_request, "密码必须同时包含字母和数字");
                }

                // 入库：DataService 内全程预处理语句 + 参数绑定，防 SQL 注入
                int rc = service_.create_user(username, password);
                if (rc == 0) {
                    json::object res;
                    res["message"] = "注册成功";
                    resp.result(boost::beast::http::status::created);
                    resp.body() = json::serialize(res);
                } else if (rc == 1) {
                    set_json_err(resp, boost::beast::http::status::conflict, "用户名已存在");
                } else {
                    set_json_err(resp, boost::beast::http::status::internal_server_error, "注册失败，请稍后重试");
                }
            } catch (const std::exception&) {
                set_json_err(resp, boost::beast::http::status::bad_request, "Invalid JSON format");
            }
        };

        // POST /api/login
        post_routes_["/login"] = [this](auto& req, auto& resp) {
            resp.set(boost::beast::http::field::content_type, "application/json");
            try {
                json::value jv = json::parse(req.body());
                auto& obj = jv.as_object();
                std::string username = obj.at("username").as_string().c_str();
                std::string password = obj.at("password").as_string().c_str();

                // 输入长度校验（防止超长输入带来的无谓 DB 负载/日志膨胀）
                if (username.size() < kUsernameLengthMIN || username.size() > kUsernameLengthMAX
                    || password.size() < kPasswordLengthMIN || password.size() > kPasswordLengthMAX) {
                    return set_json_err(resp, boost::beast::http::status::bad_request, "Invalid username or password length");
                }

                // 登录失败限流（Redis 启用时生效；未启用则跳过，不阻塞服务）
                std::string fail_key = "login_fail:" + username;
                if (redis_store::enabled() && redis_store::get_long(fail_key) >= kLoginFailThreshold) {
                    return set_json_err(resp, boost::beast::http::status::too_many_requests,
                                        "Too many login attempts, try again later");
                }

                bool valid = false, admin = false;
                long user_id = 0;
                service_.authenticate_user(username, password, user_id, admin, valid);

                if (valid) {
                    // 登录成功：清除失败计数
                    if (redis_store::enabled())
                        redis_store::cache_del(fail_key);

                    json::object res_obj;
                    std::string role = admin ? "admin" : "user";
                    std::string access_token = build_jwt_token(jwt_secret_, jwt_access_exp_seconds_, user_id, role, "access");
                    std::string refresh_token = build_jwt_token(jwt_secret_, jwt_refresh_exp_seconds_, user_id, role, "refresh");
                    if (access_token.empty() || refresh_token.empty()) {
                        LOG_ERR("Login succeeded but token signing failed (HS256). Check libjwt setup.");
                        return set_json_err(resp, boost::beast::http::status::internal_server_error, "Token signing failed, please check server config");
                    }
                    res_obj["accessToken"] = access_token;
                    res_obj["refreshToken"] = refresh_token;
                    res_obj["role"] = role;
                    res_obj["expiresIn"] = jwt_access_exp_seconds_;
                    resp.result(boost::beast::http::status::ok);
                    resp.body() = json::serialize(res_obj);
                } else {
                    if (redis_store::enabled())
                        redis_store::incr_with_expire(fail_key, kLoginFailWindowSec);
                    set_json_err(resp, boost::beast::http::status::unauthorized, "Invalid username or password");
                }
            } catch (const std::exception& e) {
                set_json_err(resp, boost::beast::http::status::bad_request, "Invalid JSON format");
            }
        };

        // POST /api/refresh
        post_routes_["/refresh"] = [this](auto& req, auto& resp) {
            resp.set(boost::beast::http::field::content_type, "application/json");

            // refresh token 黑名单检查（登出后失效；Redis 未启用时跳过）
            {
                std::string raw_token;
                if (redis_store::enabled() && extract_token(req, raw_token)
                    && redis_store::key_exists(token_blacklist_key(raw_token))) {
                    return set_json_err(resp, boost::beast::http::status::unauthorized, "Token has been revoked");
                }
            }

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

        // 登出：把当前 refresh token 拉黑（有效期对齐 refresh token 的 30 天）
        // POST /api/logout（携带 refresh token 即可，无需校验有效性）
        post_routes_["/logout"] = [](auto& req, auto& resp) {
            resp.set(boost::beast::http::field::content_type, "application/json");
            std::string raw_token;
            if (!extract_token(req, raw_token)) {
                return set_json_err(resp, boost::beast::http::status::unauthorized, "Missing token");
            }
            if (redis_store::enabled()) {
                redis_store::cache_setex(token_blacklist_key(raw_token), 30L * 24 * 3600, "1");
            }
            json::object res;
            res["message"] = "Logged out";
            resp.result(boost::beast::http::status::ok);
            resp.body() = json::serialize(res);
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
                // articleId 兼容数字与字符串两种前端写法
                int articleId = 0;
                if (obj.at("articleId").is_int64()) {
                    articleId = static_cast<int>(obj.at("articleId").as_int64());
                } else if (obj.at("articleId").is_string()) {
                    try { articleId = std::stoi(obj.at("articleId").as_string().c_str()); }
                    catch (...) { return set_json_err(resp, boost::beast::http::status::bad_request, "Invalid articleId"); }
                } else {
                    return set_json_err(resp, boost::beast::http::status::bad_request, "Invalid articleId");
                }
                if (articleId <= 0) {
                    return set_json_err(resp, boost::beast::http::status::bad_request, "Invalid articleId");
                }
                std::string content = obj.at("content").as_string().c_str();
                // 按字符计数（comment.content 为 text 列，按字符计）
                if (utf8_char_count(content) > kContentLength) {
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

                // 按"字符"计数校验（title 列 varchar(255)、content 列 longtext 均按字符计）
                if (utf8_char_count(title) > kTitleLength || utf8_char_count(content) > kContentLength) {
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
                //文章列表（分页支持）：/articles 或 /articles?page=&pageSize=
                //（/articles/{id} 动态分支在下方按前缀匹配，不受此处影响）
                if (route_target == "/articles"
                    || (route_target.size() > 9 && route_target.substr(0, 9) == "/articles" && route_target[9] == '?'))
                {
                    long page = 1, page_size = 100;
                    // 解析 query：page / pageSize（is_numeric 白名单校验后转数字，防注入/超大值）
                    if (route_target.size() > 10) {
                        std::string_view qs = route_target.substr(10);
                        std::size_t pos = 0;
                        while (pos < qs.size()) {
                            std::size_t amp = qs.find('&', pos);
                            std::string_view kv = qs.substr(pos, (amp == std::string_view::npos) ? qs.size() - pos : amp - pos);
                            pos = (amp == std::string_view::npos) ? qs.size() : amp + 1;
                            auto eq = kv.find('=');
                            if (eq == std::string_view::npos) continue;
                            std::string_view k = kv.substr(0, eq);
                            std::string_view v = kv.substr(eq + 1);
                            if (!is_numeric(v)) continue;
                            long n = std::atol(std::string(v).c_str());
                            if (k == "page" && n > 0) page = n;
                            else if (k == "pageSize" && n > 0) page_size = n;
                        }
                    }
                    if (page_size > 100) page_size = 100; // 单页上限

                    // 列表缓存（cache-aside，TTL 30s）：写路径(发/改/删/点赞/浏览)已使首页键失效，
                    // 其余分页靠短 TTL 容忍延迟
                    const std::string list_key = "articles:list:" + std::to_string(page) + ":" + std::to_string(page_size);
                    std::string list_body;
                    if (redis_store::enabled()) {
                        auto hit = redis_store::cache_get(list_key);
                        if (hit.has_value()) list_body = *hit;
                    }
                    if (list_body.empty()) {
                        json::array articles_list;
                        service_.fetch_articles(articles_list, page_size, (page - 1) * page_size);
                        list_body = json::serialize(articles_list);
                        if (redis_store::enabled())
                            redis_store::cache_setex(list_key, 30, list_body);
                    }
                    resp.result(boost::beast::http::status::ok);
                    resp.body() = std::move(list_body);
                    break;
                }
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
                    // 获取评论 GET /api/comments?articleId=...（按前缀长度截取，避免魔法数字；遇多余 query 按 & 截断）
                    constexpr std::string_view kCommentsPrefix = "/comments?articleId=";
                    std::string_view rest = route_target.substr(kCommentsPrefix.size());
                    auto amp = rest.find('&');
                    if (amp != std::string_view::npos)
                        rest = rest.substr(0, amp);
                    std::string id_str(rest);
                    // 评论列表缓存（TTL 60s；发表评论即时失效，删除评论靠 TTL 容忍）
                    const std::string comments_key = "comments:" + id_str;
                    std::string comments_body;
                    if (redis_store::enabled()) {
                        auto hit = redis_store::cache_get(comments_key);
                        if (hit.has_value()) comments_body = *hit;
                    }
                    if (comments_body.empty()) {
                        json::array comments;
                        service_.fetch_comments(id_str, comments);
                        comments_body = json::serialize(comments);
                        if (redis_store::enabled())
                            redis_store::cache_setex(comments_key, 60, comments_body);
                    }
                    resp.result(boost::beast::http::status::ok);
                    resp.body() = std::move(comments_body);
                }
                else if (route_target.starts_with("/user/stats"))
                {
                    AuthContext auth;
                    if (!check_auth_and_get_context(req, jwt_secret_, auth, false)) {
                        set_json_err(resp, boost::beast::http::status::unauthorized, "Unauthorized");
                        break;
                    }
                    // 用户统计缓存（TTL 30s，弱一致可接受）
                    const std::string stats_key = "user:stats:" + std::to_string(auth.user_id);
                    std::string stats_body;
                    if (redis_store::enabled()) {
                        auto hit = redis_store::cache_get(stats_key);
                        if (hit.has_value()) stats_body = *hit;
                    }
                    if (stats_body.empty()) {
                        json::object res_obj;
                        service_.fetch_user_stats(auth.user_id, res_obj);
                        stats_body = json::serialize(res_obj);
                        if (redis_store::enabled())
                            redis_store::cache_setex(stats_key, 30, stats_body);
                    }
                    resp.result(boost::beast::http::status::ok);
                    resp.body() = std::move(stats_body);
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
                                std::string key = get_client_key(req) + "|" + article_id;
                                // 去重（10 秒窗口）：
                                // - Redis 启用时用 SETNX EX（跨进程一致，多实例部署也有效）
                                // - 否则回退进程内分片去重表
                                bool do_inc;
                                if (redis_store::enabled()) {
                                    do_inc = redis_store::set_nx_ex("view:" + key, "1", 10);
                                } else {
                                    auto now = std::chrono::steady_clock::now();
                                    do_inc = g_view_dedup.should_count(key, now);
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

                        if (utf8_char_count(title) > kTitleLength || utf8_char_count(content) > kContentLength) {
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

                        if (has_title && utf8_char_count(title) > kTitleLength) {
                            set_json_err(resp, boost::beast::http::status::bad_request, "Title too long");
                            break;
                        }
                        if (has_content && utf8_char_count(content) > kContentLength) {
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
