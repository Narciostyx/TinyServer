#include "http.hpp"

#include <boost/beast.hpp>
#include <cstdint>
#include <sstream>

namespace project
{
	std::string serialize_http_response(HttpResponse& resp)
	{
		if (resp.find(boost::beast::http::field::content_length) == resp.end())
			resp.set(boost::beast::http::field::content_length, std::to_string(resp.body().size()));
		// 默认 close；若调用方已显式设置（如 keep-alive），则不再覆盖
		if (resp.find(boost::beast::http::field::connection) == resp.end())
			resp.set(boost::beast::http::field::connection, "close");
		if (resp.find(boost::beast::http::field::content_type) == resp.end())
			resp.set(boost::beast::http::field::content_type, "text/plain; charset=utf-8");

		resp.prepare_payload();

		std::ostringstream oss;
		oss << resp;
		return oss.str();
	}

	ParseResult parse_http_request(const std::string& raw, HttpRequest& out)
	{
		// 显式限制请求体大小（本项目业务字段上限约 8KB，64KB 足够且防 DoS 撑爆内存）。
		// 超限时 Beast 返回 error::body_limit，会走 Error 分支回 4xx。
		constexpr std::uint64_t kBodyLimit = 64 * 1024;

		boost::beast::error_code ec;
		boost::beast::http::request_parser<boost::beast::http::string_body> parser;
		parser.body_limit(kBodyLimit);
		parser.eager(true);

		parser.put(boost::asio::buffer(raw), ec);

		// 数据不完整（半包）：保留数据等待更多字节
		if (ec == boost::beast::http::error::need_more)
			return ParseResult::NeedMore;

		// 其他解析错误（含 body 超限）：请求非法
		if (ec)
			return ParseResult::Error;

		// 没有报错但还没解析完，同样视为半包
		if (!parser.is_done())
			return ParseResult::NeedMore;

		out = parser.get();
		return ParseResult::Ok;
	}
}
