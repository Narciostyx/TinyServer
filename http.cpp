#include "http.hpp"

#include <ctime>

#include <boost/json.hpp>

namespace project
{
	namespace
	{
		// 固定英文表：避免依赖 locale
		constexpr const char* kWeekdayNames[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
		constexpr const char* kMonthNames[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
												  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

		std::string to_two_digits(int v)
		{
			std::string s = std::to_string(v);
			if (s.size() < 2)
				s.insert(s.begin(), '0');
			return s;
		}
	}

	std::string make_http_date()
	{
		const std::time_t now = std::time(nullptr);
		std::tm tm_buf{};
		// gmtime_r 失败时退化为 Unix epoch，保证仍然返回一个合法格式的时间串
		if (::gmtime_r(&now, &tm_buf) == nullptr)
			return std::string("Thu, 01 Jan 1970 00:00:00 GMT");

		const int wday = (tm_buf.tm_wday >= 0 && tm_buf.tm_wday < 7) ? tm_buf.tm_wday : 0;
		const int mon = (tm_buf.tm_mon >= 0 && tm_buf.tm_mon < 12) ? tm_buf.tm_mon : 0;

		std::string out;
		out.reserve(29);
		out += kWeekdayNames[wday];
		out += ", ";
		out += to_two_digits(tm_buf.tm_mday);
		out += ' ';
		out += kMonthNames[mon];
		out += ' ';
		out += std::to_string(tm_buf.tm_year + 1900);
		out += ' ';
		out += to_two_digits(tm_buf.tm_hour);
		out += ':';
		out += to_two_digits(tm_buf.tm_min);
		out += ':';
		out += to_two_digits(tm_buf.tm_sec);
		out += " GMT";
		return out;
	}

	void apply_default_response_headers(HttpResponse& resp,
									   const std::string& server_header,
									   const std::string& default_content_type)
	{
		// 取值来自配置（Server_header / Default_content_type）；配置成空串表示"不发送该头"
		if (!server_header.empty() && resp.find(boost::beast::http::field::server) == resp.end())
			resp.set(boost::beast::http::field::server, server_header);

		// HTTP/1.1 规范要求源服务器必须发送 Date
		if (resp.find(boost::beast::http::field::date) == resp.end())
			resp.set(boost::beast::http::field::date, make_http_date());

		if (!default_content_type.empty() && resp.find(boost::beast::http::field::content_type) == resp.end())
			resp.set(boost::beast::http::field::content_type, default_content_type);
	}

	void set_json_error(HttpResponse& resp, boost::beast::http::status status, const std::string& msg)
	{
		resp.result(status);
		boost::json::object err_obj;
		err_obj["message"] = msg;
		resp.set(boost::beast::http::field::content_type, "application/json; charset=utf-8");
		resp.body() = boost::json::serialize(err_obj);
	}

} // namespace project
