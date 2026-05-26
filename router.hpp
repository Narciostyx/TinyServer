#pragma once

#include <unordered_map>
#include <string_view>
#include <functional>
#include <boost/beast/http.hpp>
#include "service.hpp"

namespace project {
    using RouteHandler = std::function<void(boost::beast::http::request<boost::beast::http::string_body>&, boost::beast::http::response<boost::beast::http::string_body>&)>;

    class Router {
    public:
        const std::string::size_type kContentLength = 8172, kTitleLength = 255, kPasswordLengthMAX = 20, kPasswordLengthMIN = 8, kUsernameLengthMAX = 15, kUsernameLengthMIN = 1;
        Router();

        void handle_request(boost::beast::http::request<boost::beast::http::string_body>& req, boost::beast::http::response<boost::beast::http::string_body>& resp);

    private:
        std::unordered_map<std::string_view, RouteHandler> get_routes_;
        std::unordered_map<std::string_view, RouteHandler> post_routes_;
        std::unordered_map<std::string_view, RouteHandler> put_routes_;
        std::unordered_map<std::string_view, RouteHandler> patch_routes_;
        std::unordered_map<std::string_view, RouteHandler> delete_routes_;
        DataService service_;

        void init_routes();
    };
}
