#ifndef _ERROR_HPP
#define _ERROR_HPP

#include <string>
#include <exception>

class Err :public std::exception
{
public:
	Err(const std::string& mes, int type = -1) :mes_(mes), type_(type) {}
	
	const char* what() const noexcept { return mes_.c_str(); }

	int getType() const { return this->type_; }

	std::string getMessage() const { return this->mes_; }

private:
	std::string mes_;
	int type_;
};

namespace project
{
	enum kErrType :int { defaultType = -1, Sql_init, Sql_conn, Reactor_init };
	inline int exit_code = 0;
}

#endif // !_ERROR_HPP
