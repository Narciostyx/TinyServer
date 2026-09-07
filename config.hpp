#ifndef _CONFIG_HPP
#define _CONFIG_HPP

#include <getopt.h>
#include <stdlib.h>

#include <iostream>
#include <filesystem>
#include <fstream>

#include "error.hpp"

namespace project
{

	// 版本号
	static const char version[] = "1.0";

	/**
	 * port：监听端口
	 * logType：日志同步（/异步）记录
	 * sqlNum：数据库连接池最大值
	 * threadNum：线程池最大值
	 */
	static const struct option long_option[] = {
		{"port",required_argument,NULL,'p'},
		{"logType",required_argument,NULL,'l'},
		{"sqlNum",required_argument,NULL,'s'},
		{"threadNum",required_argument,NULL,'t'},
		{"help",no_argument,NULL,'h'},
		{"version",no_argument,NULL,'v'},
		{NULL,NULL,NULL,NULL}
	};

	static const char short_option[] = "p:l:s:t:hv";

	// 默认池内线程最大数
	constexpr int kMaxThreadNum = 3000;
	// 默认池内数据库连接最大数
	constexpr int kMaxSqlNum = 3000;

	// 命名行解析类
	class Config
	{
	public:
		// 启动相关
		int port, sql_num, thread_num;

		// 日志相关
		int log_type, log_buffer_size, log_queue_size;
		std::string log_path;
		long log_row_flush, log_row_max;

		// 数据库相关
		int dbport;
		std::string address, username, passwd, dbname;
		bool retry;

		// reactor模型相关
		int sub_reactor_num, time_out, max_listening;

		// jwt认证相关
		std::string jwt_secret;
		long jwt_access_exp_seconds;
		long jwt_refresh_exp_seconds;
		// CORS 允许来源白名单；默认 "*"（开发便利）。生产建议通过环境变量 TINYSERVER_CORS_ORIGIN 收紧。
		std::string cors_origin = "*";

		Config()
		{
			port = 8080;
			sql_num = 15;
			thread_num = 10;
			log_type = 0;
			log_buffer_size = 1024;
			log_queue_size = 1024;
			log_path = "./TinyServerVar/log/";
			log_row_max = 50000;
			log_row_flush = 200;
			address = "127.0.0.1";
			dbport = 3306;
			username = "webdb";
			passwd = "webdb";
			dbname = "webdatabase";
			retry = false;
			sub_reactor_num = 10;
			time_out = 600;
			max_listening = 5000;
			jwt_secret = "K7gKCq9pMn4xL2vR8wYzF5tJ3hN6sA0dUeBmXcPiOjI=";
			jwt_access_exp_seconds = 60 * 60 * 24 * 7;
			jwt_refresh_exp_seconds = 60 * 60 * 24 * 30;
		}
		/**
		 * 解析命令行参数.
		 * 
		 * \param argc：参数个数
		 * \param argv：参数数组
		 */
		void parseArg(int, char* []);

	private:
		// 打印帮助
		void printHelp()
		{
			std::cout << "Usage:.\\TinyServer Options\n"
				<< "Options:\n"
				<< "\t-p, --port	Set the server's port\n"
				<< "\t-l, --logType	Select the pattern of logging, 0(default) is sync and 1 is async.\n"
				<< "\t-s, --sqlNum	Set the maximum value of the connections in sql pool.\n"
				<< "\t-t, --threadNum	Set the maximum value of the threads in thread pool.\n"
				<< "\t-v, --version	Display the version information.\n"
				<< "\t-h, --help	Display the help information.\n"
				<< "Following are some specific attributions in the configuration file:\n"
				<< "\tlog_row_max is the maximum lines in a log file.\n"
				<< "\tlog_row_flush is the frequency of writing into the file which achieves the limit in the async mode.\n"
				<< "\tsub_reactor_num is the maximum of subReactors running in the reactor mode.\n"
				<< "\tjwt_secret is the key used in the jwt encryption."
				<< std::endl;
		}
		// 打印版本
		void printVersion() { std::cout << "Current version:" << std::string(version) << std::endl; }
		// 打印配置信息
		void printInfo()
		{
			std::cout << "Port:" << port << "\nlogType:" << ((log_type == 0) ? std::string("sync\n") : std::string("async\n"))
				<< "Maximum database connections:" << sql_num << "\nMaximum threads in threadpool:" << thread_num << "\nMaximum subReactor:" << sub_reactor_num
				<< "\nTotal " << std::to_string(thread_num + sub_reactor_num + ((log_type == 0) ? 0 : 1) + 1)
				<< " threads will be running."
				<< std::endl;
		}
		// 创建默认配置文件
		void createDefaultConfig(std::fstream& file,std::filesystem::path path)
		{
			file.open(path,std::ios::out);
			if(!file.is_open())
			{
				perror("Open failed");
				exit(exit_code = -1);
			}
			file << "[Config]"
				<< "\nPort " << port
				<< "\nSQL_num " << sql_num
				<< "\nThread_num " << thread_num
				<< "\nLog_type " << log_type
				<< "\nLog_buffer_size " << log_buffer_size
				<< "\nLog_queue_size " << log_queue_size
				<< "\nLog_path " << log_path
				<< "\nLog_row_max " << log_row_max
				<< "\nLog_row_flush " << log_row_flush
				<< "\nDB_address " << address
				<< "\nDB_port " << dbport
				<< "\nDB_username " << username
				<< "\nDB_passwd " << passwd
				<< "\nDB_dbname " << dbname
				<< "\nDB_retry " << ( retry ? 1 : 0 )
				<< "\nSub_reactor_count " << sub_reactor_num
				<< "\nTime_out_connection " << time_out
				<< "\nJwt_secret " << jwt_secret
				<< "\nJwt_access_exp_seconds " << jwt_access_exp_seconds
				<< "\nJwt_refresh_exp_seconds " << jwt_refresh_exp_seconds;
			file.close();
		}
		// 从文件载入配置
		void loadConfigFromFile()
		{
			std::filesystem::path path = "./TinyServerVar";
			std::filesystem::path cfgPath = path / "config";
			std::filesystem::path bakPath = path / "config.bak";
			std::fstream file;

			// 判断路径是否存在，不存在则尝试创建
			if (!std::filesystem::exists(path)) {
				if (!std::filesystem::create_directories(path)) {
					perror("Can't create the path:./ServerConfig");
					exit(exit_code = -1);
				}
			}

			// 如果配置文件不存在，尝试从备份恢复，否则创建默认配置文件
			if (!std::filesystem::exists(cfgPath)) {
				if (std::filesystem::exists(bakPath)) {
					if (!std::filesystem::copy_file(bakPath, cfgPath, std::filesystem::copy_options::overwrite_existing)) {
						std::cout << "Load config from backup failed.\nCreate default config file.\n";
						createDefaultConfig(file, cfgPath);
						return;
					}
				}
				else {
					// 没有备份，直接创建默认配置文件
					createDefaultConfig(file, cfgPath);
					return;
				}
			}

			//读取配置文件内容
			file.open(cfgPath, std::ios::in);
			if (!file.is_open()) {
				perror("Open failed");
				exit(exit_code = -1);
			}

			std::string key, val;

			// parse_xxx均为辅助函数，负责执行安全类型转换

			auto parse_int = [](const std::string& s, int& out) -> bool {
				try {
					size_t idx = 0;
					long v = std::stol(s, &idx);
					if (idx != s.size()) return false;
					out = (int)v;
					return true;
				}
				catch (...) { return false; }
				};
			auto parse_long = [](const std::string& s, long& out) -> bool {
				try {
					size_t idx = 0;
					long v = std::stol(s, &idx);
					if (idx != s.size()) return false;
					out = v;
					return true;
				}
				catch (...) { return false; }
				};
			auto parse_bool = [](const std::string& s, bool& out) -> bool {
				if (s == "1" || s == "true" || s == "True" || s == "TRUE") { out = true; return true; }
				if (s == "0" || s == "false" || s == "False" || s == "FALSE") { out = false; return true; }
				return false;
				};

			// 终止函数
			auto exitAndReport = [](const std::string& s)
				{
					std::cout << "Illegal value for " << s << " in the configuration file.\n";
					exit(exit_code = -1);
				};

			while (file >> key)
			{
				// 忽略节头，例如 [Config]
				if (!key.empty() && key.front() == '[')
				{
					std::string rest;
					std::getline(file, rest);
					continue;
				}

				// 忽略注释行（以#开头）
				if (!key.empty() && key.front() == '#')
				{
					std::string rest;
					std::getline(file, rest);
					continue;
				}

				if (!(file >> val))
				{
					// 读取不到值说明格式不对，跳过本行
					file.clear();
					std::string rest;
					std::getline(file, rest);
					continue;
				}

				if (key == "Port")
				{
					int v = 0;
					if (!parse_int(val, v) || v <= 0 || v > 65535)
						exitAndReport("port");
					port = v;
				}
				else if (key == "SQL_num")
				{
					int v = 0;
					if (!parse_int(val, v) || v <= 0 || v > kMaxSqlNum)
						exitAndReport("SQL_num");
					sql_num = v;
				}
				else if (key == "Thread_num")
				{
					int v = 0;
					if (!parse_int(val, v) || v <= 0 || v > kMaxThreadNum)
						exitAndReport("Thread_num");
					thread_num = v;
				}
				else if (key == "Log_type")
				{
					int v = 0;
					if (!parse_int(val, v) || v < 0 || v > 1)
						exitAndReport("Log_type");
					log_type = v;
				}
				else if (key == "Log_buffer_size")
				{
					int v = 0;
					if (!parse_int(val, v) || v <= 0)
						exitAndReport("Log_buffer_size");
					log_buffer_size = v;
				}
				else if (key == "Log_queue_size")
				{
					int v = 0;
					if (!parse_int(val, v) || v <= 0)
						exitAndReport("Log_queue_size");
					log_queue_size = v;
				}
				else if (key == "Log_path")
				{
					log_path = val;
					if (!std::filesystem::is_directory(val))
						exitAndReport("Log_path");
				}
				else if (key == "Log_row_max")
				{
					long v = 0;
					if (!parse_long(val, v) || v <= 0)
						exitAndReport("Log_row_max");
					log_row_max = v;
				}
				else if (key == "Log_row_flush")
				{
					long v = 0;
					if (!parse_long(val, v) || v <= 0)
						exitAndReport("Log_row_flush");
					log_row_flush = v;
				}
				else if (key == "DB_address")
				{
					address = val;
				}
				else if (key == "DB_port")
				{
					int v = 0;
					if (!parse_int(val, v) || v <= 0 || v > 65535)
						exitAndReport("DB_port");
					dbport = v;
				}
				else if (key == "DB_username")
				{
					username = val;
				}
				else if (key == "DB_passwd")
				{
					passwd = val;
				}
				else if (key == "DB_dbname")
				{
					dbname = val;
				}
				else if (key == "DB_retry")
				{
					bool v = false;
					if (!parse_bool(val, v))
						exitAndReport("DB_retry");
					retry = v;
				}
				else if (key == "Sub_reactor_count")
				{
					int v = 0;
					if (!parse_int(val, v) || v <= 0 || v > 10000)
						exitAndReport("Sub_reactor_count");
					sub_reactor_num = v;
				}
				else if (key == "Time_out_connection")
				{
					int v = 0;
					if (!parse_int(val, v) || v <= 0)
						exitAndReport("Time_out_connection");
					time_out = v;
				}
				else if (key == "Max_listening_connection")
				{
					int v = 0;
					if (!parse_int(val, v) || v <= 0)
						exitAndReport("Max_listening_connection");
					max_listening = v;
				}
				else if (key == "Jwt_secret")
				{
					jwt_secret = val;
				}
				else if (key == "Jwt_access_exp_seconds")
				{
					long v = 0;
					if (!parse_long(val, v) || v <= 0)
						exitAndReport("Jwt_access_exp_seconds");
					jwt_access_exp_seconds = v;
				}
				else if (key == "Jwt_refresh_exp_seconds")
				{
					long v = 0;
					if (!parse_long(val, v) || v <= 0)
						exitAndReport("Jwt_refresh_exp_seconds");
					jwt_refresh_exp_seconds = v;
				}
				else
				{
					continue;
				}
			}
			file.close();
		}
	};
}
#endif