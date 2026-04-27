#include "http.hpp"

#include <boost/beast.hpp>
#include <sstream>

namespace project
{
	std::string HttpResponse::to_string() const
	{
		boost::beast::http::response<boost::beast::http::string_body> res;
		res.version(11); // HTTP/1.1
		res.result(status);
		res.reason(reason);
		for (const auto& [k, v] : headers)
		{
			res.set(k, v);
		}

		if (res.find(boost::beast::http::field::content_length) == res.end())
			res.set(boost::beast::http::field::content_length, std::to_string(body.size()));
		if (res.find(boost::beast::http::field::connection) == res.end())
			res.set(boost::beast::http::field::connection, "close");
		if (res.find(boost::beast::http::field::content_type) == res.end())
			res.set(boost::beast::http::field::content_type, "text/plain; charset=utf-8");

		res.body() = body;
		res.prepare_payload();

		std::ostringstream oss;
		oss << res;
		return oss.str();
	}

	bool parse_http_request(const std::string& raw, HttpRequest& out)
	{
		boost::beast::error_code ec;
		boost::beast::http::request_parser<boost::beast::http::string_body> parser;
		parser.eager(true);

		parser.put(boost::asio::buffer(raw), ec);

		if (ec && ec != boost::beast::http::error::need_more)
			return false;

		if (!parser.is_done())
			return false;

		const auto& req = parser.get();
		out.method = std::string(req.method_string());
		out.path = std::string(req.target());
		out.version = "HTTP/" + std::to_string(req.version() / 10) + "." + std::to_string(req.version() % 10);

		out.headers.clear();
		for (const auto& field : req)
		{
			out.headers[std::string(field.name_string())] = std::string(field.value());
		}

		out.body = req.body();
		return true;
	}
}
