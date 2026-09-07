#pragma once

#include <unordered_map>
#include <string_view>
#include <functional>
#include <boost/beast/http.hpp>
#include "config.hpp"
#include "service.hpp"

namespace project {
    constexpr std::string::size_type kContentLength = 8172, kTitleLength = 255, kPasswordLengthMAX = 20, kPasswordLengthMIN = 8, kUsernameLengthMAX = 15, kUsernameLengthMIN = 1;
    using RouteHandler = std::function<void(boost::beast::http::request<boost::beast::http::string_body>&, boost::beast::http::response<boost::beast::http::string_body>&)>;

    class Router {
    public:
        Router();
        explicit Router(const Config& cfg);
        // 路由 handler 内捕获了 this（成员函数指针），任何拷贝/移动都会让闭包里的 this 悬垂，
        // 因此 Router 不可拷贝、不可移动；配置变更请走 apply_config()
        Router(const Router&) = delete;
        Router& operator=(const Router&) = delete;
        Router(Router&&) = delete;
        Router& operator=(Router&&) = delete;

        /**
         * 应用配置（JWT 密钥与有效期）。
         * 路由表在默认构造时已建立，本方法只刷新配置值；
         * 避免"临时 Router 移动赋值替换成员"导致 handler 内 this 指向已析构对象的悬垂崩溃。
         * \param cfg 服务器配置
         */
        void apply_config(const Config& cfg);

        void handle_request(boost::beast::http::request<boost::beast::http::string_body>& req, boost::beast::http::response<boost::beast::http::string_body>& resp);

    private:
        std::unordered_map<std::string_view, RouteHandler> get_routes_;
        std::unordered_map<std::string_view, RouteHandler> post_routes_;
        std::unordered_map<std::string_view, RouteHandler> put_routes_;
        std::unordered_map<std::string_view, RouteHandler> patch_routes_;
        std::unordered_map<std::string_view, RouteHandler> delete_routes_;
        DataService service_;
        Config config_;
        //jwt加密密钥
        std::string jwt_secret_;
        long long jwt_access_exp_seconds_ = 0;
        long long jwt_refresh_exp_seconds_ = 0;

        void init_routes();
    };
}
