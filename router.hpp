#pragma once

#include <unordered_map>
#include <string_view>
#include <functional>
#include <boost/beast/http.hpp>
#include <mysql/mysql.h>

namespace project {
    using RouteHandler = std::function<void(boost::beast::http::request<boost::beast::http::string_body>&, boost::beast::http::response<boost::beast::http::string_body>&)>;

    class Router {
    public:
        Router();

        void handle_request(boost::beast::http::request<boost::beast::http::string_body>& req, boost::beast::http::response<boost::beast::http::string_body>& resp);

    private:
        std::unordered_map<std::string_view, RouteHandler> get_routes_;
        std::unordered_map<std::string_view, RouteHandler> post_routes_;
        std::unordered_map<std::string_view, RouteHandler> put_routes_;
        std::unordered_map<std::string_view, RouteHandler> patch_routes_;
        std::unordered_map<std::string_view, RouteHandler> delete_routes_;

        void init_routes();
        bool db_query(const std::string& sql, std::function<void(MYSQL_RES*)>&& callback) noexcept;
        bool db_execute(const std::string& sql, std::function<void(long)>&& callback) noexcept;

        // SQL 注入转义工具函数
        std::string escape_sql_string(const std::string& input);
    };
}
