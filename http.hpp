#ifndef _HTTP_HPP
#define _HTTP_HPP

#include <string>
#include <unordered_map>

namespace project
{
	// HTTP请求结构（最简版）
	struct HttpRequest
	{
		std::string method;
		std::string path;
		std::string version;
		std::unordered_map<std::string, std::string> headers;
		std::string body;
	};

	// HTTP响应结构（最简版）
	struct HttpResponse
	{
		int status = 200;
		std::string reason = "OK";
		std::unordered_map<std::string, std::string> headers;
		std::string body;

		std::string to_string() const;
	};

	// 解析HTTP请求（最简版，支持Content-Length）
	// 返回：
	// - true：解析成功（out被填充）
	// - false：数据不完整或格式错误
	bool parse_http_request(const std::string& raw, HttpRequest& out);
}

#endif
