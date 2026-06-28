#pragma once

#include <unordered_map>
#include <string_view>
#include <functional>
#include <boost/beast/http.hpp>
#include "config.hpp"
#include "service.hpp"

namespace project {
    using RouteHandler = std::function<void(boost::beast::http::request<boost::beast::http::string_body>&, boost::beast::http::response<boost::beast::http::string_body>&)>;

    class Router {
    public:
        std::string::size_type kContentLength = 8172, kTitleLength = 255, kPasswordLengthMAX = 20, kPasswordLengthMIN = 8, kUsernameLengthMAX = 15, kUsernameLengthMIN = 1;
        Router();
        explicit Router(const Config& cfg);
        Router(const Router&) = delete;
        Router& operator=(const Router&) = delete;
        Router(Router&&) = default;
        Router& operator=(Router&& other) = default;

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
