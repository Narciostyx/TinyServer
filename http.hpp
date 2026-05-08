#ifndef _HTTP_HPP
#define _HTTP_HPP

#include <string>
#include <boost/beast/http.hpp>

namespace project
{
	// 使用 Boost.Beast 提供的 HTTP 请求和响应类型
	using HttpRequest = boost::beast::http::request<boost::beast::http::string_body>;
	using HttpResponse = boost::beast::http::response<boost::beast::http::string_body>;

	// 解析HTTP请求
	// 返回：
	// - true：解析成功（out被填充）
	// - false：数据不完整或格式错误
	bool parse_http_request(const std::string& raw, HttpRequest& out);

	// 将 HttpResponse 序列化为字符串
	std::string serialize_http_response(HttpResponse& resp);
}

#endif
