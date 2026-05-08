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

	//版本号
	static const char version[] = "1.0";

	//长选项结构体
	static const struct option long_option[] = {
		{"port",required_argument,NULL,'p'},
		{"logType",required_argument,NULL,'l'},
		{"sqlNum",required_argument,NULL,'s'},
		{"threadNum",required_argument,NULL,'t'},
		{"help",no_argument,NULL,'h'},
		{"version",no_argument,NULL,'v'},
		{NULL,NULL,NULL,NULL}
	};

	//短选项
	static const char short_option[] = "p:l:s:t:hv";

	//默认池内线程最大数
	constexpr int kMaxThreadNum = 3000;
	//默认池内数据库连接最大数
	constexpr int kMaxSqlNum = 3000;

	//命名行解析类
	class Config
	{
	public:
		int port, sql_num, thread_num;
		int log_type, log_buffer_size, log_queue_size;
		std::string log_path;
		long log_row_flush, log_row_max;
		int dbport;
		std::string address, username, passwd, dbname;
		bool retry;
		int sub_reactor_num, time_out, max_listening;

		Config()
		{
			port = 8080;
			sql_num = 150;
			thread_num = 100;
			log_type = 0;
			log_buffer_size = 1024;
			log_queue_size = 1024;
			log_path = "./log/";
			log_row_max = 50000;
			log_row_flush = 1;
			address = "127.0.0.1";
			dbport = 3306;
			username = "webdb";
			passwd = "webdb";
			dbname = "webdatabase";
			retry = false;
			sub_reactor_num = 10;
			time_out = 600;
			max_listening = 5000;
		}
		//解析命令行参数
		void parseArg(int, char* []);

	private:
		//打印帮助
		void printHelp()
		{
			std::cout << "Usage:.\\TinyServer Options\n"
				<< "Options:\n"
				<< "	-p, --port	Set the server's port\n"
				<< "	-l, --logType	Select the pattern of logging, 0(default) is sync and 1 is async.\n"
				<< "	-s, --sqlNum	Set the maximum value of the connections in sql pool.\n"
				<< "	-t, --threadNum	Set the maximum value of the threads in thread pool.\n"
				<< "	-v, --version	Display the version infomation.\n"
				<< "	-h, --help	Display the help information."
				<< std::endl;
		}
		//打印版本
		void printVersion() { std::cout << "Current version:" << std::string(version) << std::endl; }
		//打印配置信息
		void printInfo()
		{
			std::cout << "Port:" << port << "\nlogType:" << ((log_type == 0) ? std::string("sync\n") : std::string("async\n"))
				<< "Maximum database connections:" << sql_num << "\nMaximum threads:" << thread_num
				<< std::endl;
		}
		void loadConfigFromFile()
		{
			std::filesystem::path path = "./Cfg";
			std::filesystem::path cfgPath = path / "config";
			std::filesystem::path bakPath = path / "config.bak";
			std::fstream file;

			// 判断路径是否存在，不存在则尝试创建
			if (!std::filesystem::exists(path)) {
				if (!std::filesystem::create_directory(path)) {
					perror("Can't create the path:./Cfg");
					exit(exit_code = -1);
				}
			}

			// 如果配置文件不存在，尝试从备份恢复，否则创建默认配置文件
			if (!std::filesystem::exists(cfgPath)) {
				if (std::filesystem::exists(bakPath)) {
					if (!std::filesystem::copy_file(bakPath, cfgPath, std::filesystem::copy_options::overwrite_existing)) {
						std::cout << "Load config from backup failed.\nCreate default config file.\n";
						// 创建默认配置文件
						file.open(cfgPath, std::ios::out);
						if (!file.is_open()) {
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
							<< "\nDB_retry " << (retry ? 1 : 0)
							<< "\nSub_reactor_count " << sub_reactor_num
							<< "\nTime_out_connection " << time_out;
						file.close();
						return;
					}
				}
				else {
					// 没有备份，直接创建默认配置文件
					file.open(cfgPath, std::ios::out);
					if (!file.is_open()) {
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
						<< "\nDB_retry " << (retry ? 1 : 0)
						<< "\nSub_reactor_count " << sub_reactor_num
						<< "\nTime_out_connection " << time_out
						<< "\nMax_listening_connection " << max_listening;
					file.close();
					return;
				}
			}

			//读取配置文件内容
			file.open(cfgPath, std::ios::in);
			if (!file.is_open()) {
				perror("Open failed");
				exit(exit_code = -1);
			}

			//键名
			std::string key;
			//值
			std::string val;

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
					{
						std::cout << "Illegal port in config file!\n";
						exit(exit_code = -1);
					}
					port = v;
				}
				else if (key == "SQL_num")
				{
					int v = 0;
					if (!parse_int(val, v) || v <= 0 || v > kMaxSqlNum)
					{
						std::cout << "Ilegal value for the SQL_num in config file.\n";
						exit(exit_code = -1);
					}
					sql_num = v;
				}
				else if (key == "Thread_num")
				{
					int v = 0;
					if (!parse_int(val, v) || v <= 0 || v > kMaxThreadNum)
					{
						std::cout << "Ilegal value for the Thread_num in config file.\n";
						exit(exit_code = -1);
					}
					thread_num = v;
				}
				else if (key == "Log_type")
				{
					int v = 0;
					if (!parse_int(val, v) || v < 0 || v > 1)
					{
						std::cout << "The argument of Log_type must be 0 or 1!\n";
						exit(exit_code = -1);
					}
					log_type = v;
				}
				else if (key == "Log_buffer_size")
				{
					int v = 0;
					if (!parse_int(val, v) || v <= 0)
					{
						std::cout << "Ilegal value for the Log_buffer_size in config file.\n";
						exit(exit_code = -1);
					}
					log_buffer_size = v;
				}
				else if (key == "Log_queue_size")
				{
					int v = 0;
					if (!parse_int(val, v) || v <= 0)
					{
						std::cout << "Ilegal value for the Log_queue_size in config file.\n";
						exit(exit_code = -1);
					}
					log_queue_size = v;
				}
				else if (key == "Log_path")
				{
					log_path = val;
				}
				else if (key == "Log_row_max")
				{
					long v = 0;
					if (!parse_long(val, v) || v <= 0)
					{
						std::cout << "Ilegal value for the Log_row_max in config file.\n";
						exit(exit_code = -1);
					}
					log_row_max = v;
				}
				else if (key == "Log_row_flush")
				{
					long v = 0;
					if (!parse_long(val, v) || v <= 0)
					{
						std::cout << "Ilegal value for the Log_row_flush in config file.\n";
						exit(exit_code = -1);
					}
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
					{
						std::cout << "Ilegal value for the DB_port in config file.\n";
						exit(exit_code = -1);
					}
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
					{
						std::cout << "Ilegal value for the DB_retry in config file.\n";
						exit(exit_code = -1);
					}
					retry = v;
				}
				else if (key == "Sub_reactor_count")
				{
					int v = 0;
					if (!parse_int(val, v) || v <= 0 || v > 10000)
					{
						std::cout << "Ilegal value for the Sub_reactor_count in config file.\n";
						exit(exit_code = -1);
					}
					sub_reactor_num = v;
				}
				else if (key == "Time_out_connection")
				{
					int v = 0;
					if (!parse_int(val, v) || v <= 0)
					{
						std::cout << "Ilegal value for the Time_out_connection in config file.\n";
						exit(exit_code = -1);
					}
					time_out = v;
				}
				else if (key == "Max_listening_connection")
				{
					int v = 0;
					if (!parse_int(val, v) || v <= 0)
					{
						std::cout << "Ilegal value for the Max_listening_connection in config file.\n";
						exit(exit_code = -1);
					}
					max_listening = v;
				}
				else
				{
					// 未知字段：忽略
				}
			}
			file.close();
		}
	};
}
#endif