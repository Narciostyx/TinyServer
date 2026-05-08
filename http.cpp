#include "http.hpp"

#include <boost/beast.hpp>
#include <sstream>

namespace project
{
	std::string serialize_http_response(HttpResponse& resp)
	{
		if (resp.find(boost::beast::http::field::content_length) == resp.end())
			resp.set(boost::beast::http::field::content_length, std::to_string(resp.body().size()));
		if (resp.find(boost::beast::http::field::connection) == resp.end())
			resp.set(boost::beast::http::field::connection, "close");
		if (resp.find(boost::beast::http::field::content_type) == resp.end())
			resp.set(boost::beast::http::field::content_type, "text/plain; charset=utf-8");

		resp.prepare_payload();

		std::ostringstream oss;
		oss << resp;
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

		out = parser.get();
		return true;
	}
}
