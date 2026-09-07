#ifndef _HTTP_HPP
#define _HTTP_HPP

#include <string>
#include <boost/beast/http.hpp>

namespace project
{
	// 使用 Boost.Beast 提供的 HTTP 请求和响应类型
	using HttpRequest = boost::beast::http::request<boost::beast::http::string_body>;
	using HttpResponse = boost::beast::http::response<boost::beast::http::string_body>;

	/**
	 * 解析结果的三种状态
	 * Ok：解析成功（out 被填充）
	 * NeedMore：数据不完整（半包），需要等待更多数据后再次解析，数据必须保留在缓冲区
	 * Error：格式错误，应直接返回 4xx
	 */
	enum class ParseResult
	{
		Ok,
		NeedMore,
		Error
	};

	/**
	 * 解析HTTP请求
	 * \param raw：原始数据
	 * \param out：实际输出格式化HTTP请求体
	 * \return 解析结果Ok、NeedMore、Error
	 */
	ParseResult parse_http_request(const std::string& raw, HttpRequest& out);

	// 
	/**
	 * 将 HttpResponse 序列化为字符串
	 * \param resp：实际输出的HTTP响应体
	 * \return 序列化HTTP响应，便于发送
	 */
	std::string serialize_http_response(HttpResponse& resp);
}

#endif
