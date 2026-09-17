#ifndef _HTTP_HPP
#define _HTTP_HPP

#include <cstdint>
#include <string>

#include <boost/beast/http.hpp>

namespace project
{
	// 使用 Boost.Beast 提供的 HTTP 请求和响应类型
	using HttpRequest = boost::beast::http::request<boost::beast::http::string_body>;
	using HttpResponse = boost::beast::http::response<boost::beast::http::string_body>;
	using HttpRequestParser = boost::beast::http::request_parser<boost::beast::http::string_body>;
	// 用于 1xx / 204 等无 body 响应（当前用于 100 Continue）
	using HttpEmptyResponse = boost::beast::http::response<boost::beast::http::empty_body>;

	// 传输层默认限额（可被配置覆盖，见 config.hpp 的 Body_limit_bytes / Header_limit_bytes）
	inline constexpr std::uint64_t kDefaultBodyLimit = 64ULL * 1024ULL;
	inline constexpr std::uint64_t kDefaultHeaderLimit = 8ULL * 1024ULL;

	/**
	 * 生成 RFC 7231 规定的 IMF-fixdate 时间串（如 "Sun, 06 Nov 1994 08:49:37 GMT"）。
	 * 说明：不使用 strftime("%a/%b")，因为其结果受 setlocale 影响（会输出中文星期名），
	 * 这里用固定英文表查表，保证 HTTP Date 头始终合法。
	 * \return GMT 时间串
	 */
	std::string make_http_date();

	/**
	 * 补齐响应默认头：Server / Date / Content-Type（已显式设置的不覆盖）。
	 * 注意：不设置 Content-Length——由调用方在使用前调用 message::prepare_payload() 决定。
	 * \param resp 待补齐的响应
	 * \param server_header Server 头取值；为空则不发该头（见配置 Server_header）
	 * \param default_content_type 缺省 Content-Type；为空则不发该头（见配置 Default_content_type）
	 */
	void apply_default_response_headers(HttpResponse& resp,
									   const std::string& server_header,
									   const std::string& default_content_type);

	/**
	 * 构造统一错误响应体：{"message":"..."} 并设置状态码（与 API_CONVENTIONS.md 约定一致）
	 * \param resp 输出响应
	 * \param status HTTP 状态码
	 * \param msg 错误提示
	 */
	void set_json_error(HttpResponse& resp, boost::beast::http::status status, const std::string& msg);

} // namespace project

#endif
