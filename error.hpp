#ifndef _ERROR_HPP
#define _ERROR_HPP

#include <string>
#include <exception>

// 全局错误类
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
	// 退出码规范（与 README.md 一致）：
	//   -1 配置/未知非正常退出；1 数据库初始化或连接；2 事件循环/传输层初始化；
	//    3 监听端口 bind/listen 失败；5 TLS 配置（证书/协议/套件）错误
	// 历史坑：旧枚举里 Sql_init 的值为 0，会让"数据库初始化失败"以退出码 0 结束
	// （看起来像成功），这里按 README 的规范重新编号。
	enum kErrType :int {
		defaultType = -1,
		Sql_init = 1,
		Sql_conn = 1,
		Reactor_init = 2,
		Listen_init = 3,
		Thread_wrong = 4,
		Redis_error = 5,
		Tls_init = 6
	};
	inline int exit_code = 0;
}

#endif // !_ERROR_HPP
