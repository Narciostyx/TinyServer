#ifndef _CONFIG_HPP
#define _CONFIG_HPP

#include <getopt.h>
#include <stdlib.h>

#include <iostream>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

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
		{"port",required_argument,nullptr,'p'},
		{"logType",required_argument,nullptr,'l'},
		{"sqlNum",required_argument,nullptr,'s'},
		{"threadNum",required_argument,nullptr,'t'},
		{"ioThreads",required_argument,nullptr,'I'},
		{"httpsPort",required_argument,nullptr,'P'},
		{"tlsCert",required_argument,nullptr,'C'},
		{"tlsKey",required_argument,nullptr,'K'},
		{"https",no_argument,nullptr,'S'},
		{"help",no_argument,nullptr,'h'},
		{"version",no_argument,nullptr,'v'},
		{nullptr,0,nullptr,0}
	};

	static const char short_option[] = "p:l:s:t:I:P:C:K:Shv";

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

		// 传输层相关（Boost.Asio）：io 线程数、监听地址、来源白名单
		int io_threads;                  // 0 = 自动：max(2, hardware_concurrency)
		std::string listen_address;      // 监听网卡地址（默认仅本机回环）
		std::string allow_peers;         // 逗号分隔 IP/CIDR 白名单（空 = 不限制）

		// HTTP / HTTPS 监听
		bool http_enable;
		bool https_enable;
		int https_port;
		bool http_redirect_to_https;     // 明文监听器只做 301 跳转到 HTTPS

		// TLS
		std::string tls_cert_file, tls_key_file, tls_ca_file;
		std::string tls_min_version;     // 1.0 / 1.1 / 1.2 / 1.3
		std::string tls_ciphers;         // TLS1.2 及以下套件
		std::string tls_ciphersuites;    // TLS1.3 套件
		bool tls_auto_self_signed;       // 证书缺失时自动生成自签证书（开发便利；生产置 0）
		bool tls_require_client_cert;    // mTLS：必须提供客户端证书
		bool tls_session_tickets;        // 会话票据（会话复用）
		std::string tls_sni_certs;       // "host=cert:key,host2=cert2:key2"

		// 限额与超时
		long body_limit_bytes;
		long header_limit_bytes;
		int request_timeout_seconds;         // 读请求体上限（Slowloris 防护）
		int tls_handshake_timeout_seconds;
		int shutdown_timeout_seconds;        // 优雅停机排空超时
		int shutdown_drain_grace_ms;         // 硬停机后等待会话关闭回调跑完的宽限

		// 监听器行为
		int listen_backlog;                  // 0 = 使用 SOMAXCONN
		bool listen_dual_stack;              // IPv6 监听地址是否同时接受 IPv4（v6_only=false）
		bool tcp_keepalive;                  // 是否对已接受连接开启 TCP keepalive

		// TLS 细节
		std::string tls_alpn;                // 逗号分隔的 ALPN 协议名（默认 "http/1.1"，上 h2 时改这里）
		int tls_verify_depth;                // 客户端证书链校验深度（mTLS）
		std::string tls_session_id_context;  // 会话复用所需的 session id context
		int tls_session_ticket_count;        // 服务端下发的 session ticket 数量

		// HTTP 响应与协议行为
		std::string server_header;           // Server 响应头取值
		std::string default_content_type;    // 未显式设置时的 Content-Type
		std::string request_id_header;       // 请求追踪头名称（回显/生成）
		int cors_max_age;                    // 预检结果缓存秒数
		std::string cors_allow_methods;
		std::string cors_allow_headers;
		std::string cors_expose_headers;

		// 业务线程池
		int thread_pool_queue_size;          // 任务队列容量（背压阈值）

		// 健康检查快路径与依赖探活
		bool health_fast_path;               // /healthz、/metrics 等是否不经业务线程池
		std::string health_fast_path_prefixes;  // 快路径前缀（逗号分隔）
		int redis_probe_interval_seconds;    // Redis 探活间隔；0 = 不探活
		int dependency_probe_fail_threshold; // 连续失败多少次才判定依赖不可用（防抖）

		// 日志
		int log_async_retry_ms;              // 异步日志队列满时的重试间隔
		int metrics_log_every_n;             // 每处理 N 个响应打印一次指标快照（0 = 关闭）

		// MySQL 连接池
		int db_connect_timeout_seconds;
		int db_max_attempts;
		int db_retry_backoff_max_ms;

		// Redis / 缓存 / 去重 / 限流
		std::string redis_uri;               // 可被环境变量 TINYSERVER_REDIS_URI 覆盖
		int redis_pool_size;
		int cache_ttl_article_seconds;
		int cache_ttl_list_seconds;
		int cache_ttl_comments_seconds;
		int cache_ttl_stats_seconds;
		int view_dedup_window_seconds;
		int view_dedup_max_per_shard;
		int article_page_size_default;
		int article_page_size_max;
		int login_fail_threshold;
		int login_fail_window_seconds;
		long refresh_token_ttl_seconds;

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

			// 传输层默认：仅本机回环 + io 线程自动
			io_threads = 0;
			listen_address = "127.0.0.1";
			allow_peers = "127.0.0.1/32,::1/128";

			// HTTPS 默认开启（端口 8443），明文 HTTP 同时保留（端口 8080）；
			// 证书缺失时会自动生成自签证书，见 Tls_auto_self_signed
			http_enable = true;
			https_enable = true;
			https_port = 8443;
			http_redirect_to_https = false;

			tls_cert_file = "./TinyServerVar/tls/server.crt";
			tls_key_file = "./TinyServerVar/tls/server.key";
			tls_ca_file.clear();
			tls_min_version = "1.2";
			tls_ciphers.clear();
			tls_ciphersuites.clear();
			tls_auto_self_signed = true;
			tls_require_client_cert = false;
			tls_session_tickets = true;
			tls_sni_certs.clear();

			body_limit_bytes = 64L * 1024L;
			header_limit_bytes = 8L * 1024L;
			request_timeout_seconds = 30;
			tls_handshake_timeout_seconds = 10;
			shutdown_timeout_seconds = 10;
			shutdown_drain_grace_ms = 500;

			// 监听器行为
			listen_backlog = 0;              // 0 => SOMAXCONN
			listen_dual_stack = true;
			tcp_keepalive = true;

			// TLS 细节
			tls_alpn = "http/1.1";
			tls_verify_depth = 4;
			tls_session_id_context = "TinyServer";
			tls_session_ticket_count = 2;

			// HTTP 响应与协议行为
			server_header = "TinyServer";
			default_content_type = "application/json; charset=utf-8";
			request_id_header = "X-Request-Id";
			cors_max_age = 600;
			cors_allow_methods = "GET, POST, PUT, PATCH, DELETE, OPTIONS";
			cors_allow_headers = "Content-Type, Authorization, X-Request-Id";
			cors_expose_headers = "X-Request-Id";

			// 业务线程池 / 日志 / 指标
			thread_pool_queue_size = 100;
			log_async_retry_ms = 10;
			metrics_log_every_n = 100;

			// 健康检查快路径与依赖探活
			health_fast_path = true;
			health_fast_path_prefixes = "/healthz,/livez,/readyz,/metrics";
			redis_probe_interval_seconds = 10;
			dependency_probe_fail_threshold = 3;

			// MySQL 连接池
			db_connect_timeout_seconds = 5;
			db_max_attempts = 10;
			db_retry_backoff_max_ms = 10000;

			// Redis / 缓存 / 去重 / 限流
			redis_uri = "tcp://127.0.0.1:6379";
			redis_pool_size = 8;
			cache_ttl_article_seconds = 300;
			cache_ttl_list_seconds = 30;
			cache_ttl_comments_seconds = 60;
			cache_ttl_stats_seconds = 30;
			view_dedup_window_seconds = 10;
			view_dedup_max_per_shard = 4096;
			article_page_size_default = 100;
			article_page_size_max = 100;
			login_fail_threshold = 5;
			login_fail_window_seconds = 300;
			refresh_token_ttl_seconds = 30L * 24L * 3600L;

			jwt_secret = "K7gKCq9pMn4xL2vR8wYzF5tJ3hN6sA0dUeBmXcPiOjI=";
			jwt_access_exp_seconds = 60 * 60 * 24 * 7;
			jwt_refresh_exp_seconds = 60 * 60 * 24 * 30;
		}

		/**
		 * 计算实际使用的 io 线程数。
		 * \return io_threads > 0 时返回其值，否则返回 max(2, hardware_concurrency)
		 */
		int resolvedIoThreads() const
		{
			if (io_threads > 0)
				return io_threads;
			const unsigned hw = std::thread::hardware_concurrency();
			return static_cast<int>(hw < 2u ? 2u : hw);
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
				<< "\t-I, --ioThreads	Set the number of Boost.Asio io threads (0 = auto).\n"
				<< "\t-P, --httpsPort	Set the HTTPS(TLS) listening port.\n"
				<< "\t-C, --tlsCert	Set the PEM certificate chain file for HTTPS.\n"
				<< "\t-K, --tlsKey	Set the PEM private key file for HTTPS.\n"
				<< "\t-S, --https	Enable the HTTPS(TLS) listener.\n"
				<< "\t-v, --version	Display the version information.\n"
				<< "\t-h, --help	Display the help information.\n"
				<< "Following are some specific attributions in the configuration file:\n"
				<< "\tlog_row_max is the maximum lines in a log file.\n"
				<< "\tlog_row_flush is the frequency of writing into the file which achieves the limit in the async mode.\n"
				<< "\tio_threads is the number of Boost.Asio io threads (0 = auto, max(2, CPU cores)).\n"
				<< "\tlisten_address / allow_peers restrict the binding address and the accepted source CIDRs.\n"
				<< "\thttps_enable / https_port / tls_cert_file / tls_key_file enable the HTTPS listener.\n"
				<< "\ttls_ca_file (+ tls_require_client_cert) enables mutual TLS (mTLS).\n"
				<< "\ttls_sni_certs maps host names to extra certificates, e.g. a.com:/p/a.crt:/p/a.key.\n"
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
				<< "Maximum database connections:" << sql_num << "\nMaximum threads in threadpool:" << thread_num
				<< "\nIO threads:" << resolvedIoThreads()
				<< "\nHTTP listener:" << (http_enable ? (listen_address + ":" + std::to_string(port)) : std::string("disabled"))
				<< "\nHTTPS listener:" << (https_enable ? (listen_address + ":" + std::to_string(https_port)
					+ " (TLS>=" + tls_min_version + (tls_require_client_cert ? ", mTLS" : "") + ")") : std::string("disabled"))
				<< "\nTotal " << std::to_string(thread_num + resolvedIoThreads() + ((log_type == 0) ? 0 : 1))
				<< " threads will be running (threadpool " << thread_num << " + io " << resolvedIoThreads()
				<< ((log_type == 0) ? "" : " + log") << ")."
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
				<< "\nMax_listening_connection " << max_listening
				// ---- 传输层（Boost.Asio / HTTP + HTTPS）----
				// 注意：本文件的解析方式是 "键 值" 逐 token 读取，
				// 因此值里不能含空格（尤其是证书路径）；含空格会截断。
				<< "\n# ---- transport (Boost.Asio) ----"
				<< "\n# io_threads: 0 = auto (max(2, CPU cores))"
				<< "\nIo_threads " << io_threads
				<< "\n# listen_address: 监听网卡地址；0.0.0.0 / :: 表示所有网卡"
				<< "\nListen_address " << listen_address
				<< "\n# allow_peers: 逗号分隔的 IP/CIDR 白名单，空表示不限制"
				<< "\nAllow_peers " << allow_peers
				<< "\nHttp_enable " << (http_enable ? 1 : 0)
				<< "\n# https_enable=1 时必须设置 tls_cert_file 与 tls_key_file"
				<< "\nHttps_enable " << (https_enable ? 1 : 0)
				<< "\nHttps_port " << https_port
				<< "\n# http_redirect_to_https=1 时明文监听器只回 301 跳转"
				<< "\nHttp_redirect_to_https " << (http_redirect_to_https ? 1 : 0)
				<< "\nTls_cert_file " << tls_cert_file
				<< "\nTls_key_file " << tls_key_file
				<< "\n# tls_auto_self_signed: 证书缺失时自动生成自签证书（开发便利；生产置 0）"
				<< "\nTls_auto_self_signed " << (tls_auto_self_signed ? 1 : 0)
				<< "\n# tls_ca_file 非空 => 启用客户端证书校验；配合 tls_require_client_cert 强制 mTLS"
				<< "\nTls_ca_file " << (tls_ca_file.empty() ? std::string("-") : tls_ca_file)
				<< "\nTls_require_client_cert " << (tls_require_client_cert ? 1 : 0)
				<< "\nTls_min_version " << tls_min_version
				<< "\nTls_ciphers " << (tls_ciphers.empty() ? std::string("-") : tls_ciphers)
				<< "\nTls_ciphersuites " << (tls_ciphersuites.empty() ? std::string("-") : tls_ciphersuites)
				<< "\nTls_session_tickets " << (tls_session_tickets ? 1 : 0)
				<< "\nTls_sni_certs " << (tls_sni_certs.empty() ? std::string("-") : tls_sni_certs)
				<< "\nBody_limit_bytes " << body_limit_bytes
				<< "\nHeader_limit_bytes " << header_limit_bytes
				<< "\nRequest_timeout_seconds " << request_timeout_seconds
				<< "\nTls_handshake_timeout_seconds " << tls_handshake_timeout_seconds
				<< "\nShutdown_timeout_seconds " << shutdown_timeout_seconds
				<< "\nShutdown_drain_grace_ms " << shutdown_drain_grace_ms
				// ---- 监听器行为 ----
				<< "\n# listen_backlog: 0 表示使用 SOMAXCONN"
				<< "\nListen_backlog " << listen_backlog
				<< "\n# listen_dual_stack: IPv6 监听地址是否同时接受 IPv4 连接"
				<< "\nListen_dual_stack " << (listen_dual_stack ? 1 : 0)
				<< "\nTcp_keepalive " << (tcp_keepalive ? 1 : 0)
				// ---- TLS 细节 ----
				<< "\n# tls_alpn: 逗号分隔的 ALPN 协议名（上 HTTP/2 时在此追加 h2）"
				<< "\nTls_alpn " << (tls_alpn.empty() ? std::string("-") : tls_alpn)
				<< "\nTls_verify_depth " << tls_verify_depth
				<< "\nTls_session_id_context " << tls_session_id_context
				<< "\nTls_session_ticket_count " << tls_session_ticket_count
				// ---- HTTP 响应与协议行为（"-" 表示留空 / 不发送该头）----
				<< "\nServer_header " << (server_header.empty() ? std::string("-") : server_header)
				<< "\nDefault_content_type " << (default_content_type.empty() ? std::string("-") : default_content_type)
				<< "\nRequest_id_header " << (request_id_header.empty() ? std::string("-") : request_id_header)
				<< "\nCors_max_age " << cors_max_age
				<< "\nCors_allow_methods " << (cors_allow_methods.empty() ? std::string("-") : cors_allow_methods)
				<< "\nCors_allow_headers " << (cors_allow_headers.empty() ? std::string("-") : cors_allow_headers)
				<< "\nCors_expose_headers " << (cors_expose_headers.empty() ? std::string("-") : cors_expose_headers)
				// ---- 业务线程池 / 日志 / 指标 ----
				<< "\nThread_pool_queue_size " << thread_pool_queue_size
				<< "\nLog_async_retry_ms " << log_async_retry_ms
				<< "\n# metrics_log_every_n: 每 N 个响应打印一次指标快照，0 = 关闭"
				<< "\nMetrics_log_every_n " << metrics_log_every_n
				<< "\n# health_fast_path: 健康检查/指标不经业务线程池（避免池饱和时探针 503）"
				<< "\nHealth_fast_path " << (health_fast_path ? 1 : 0)
				<< "\nHealth_fast_path_prefixes " << (health_fast_path_prefixes.empty() ? std::string("-") : health_fast_path_prefixes)
				<< "\n# redis_probe_interval_seconds: Redis 探活间隔，0 = 不探活"
				<< "\nRedis_probe_interval_seconds " << redis_probe_interval_seconds
				<< "\nDependency_probe_fail_threshold " << dependency_probe_fail_threshold
				// ---- MySQL 连接池 ----
				<< "\nDB_connect_timeout_seconds " << db_connect_timeout_seconds
				<< "\nDB_max_attempts " << db_max_attempts
				<< "\nDB_retry_backoff_max_ms " << db_retry_backoff_max_ms
				// ---- Redis / 缓存 / 去重 / 限流 ----
				<< "\n# redis_uri 可被环境变量 TINYSERVER_REDIS_URI 覆盖"
				<< "\nRedis_uri " << redis_uri
				<< "\nRedis_pool_size " << redis_pool_size
				<< "\nCache_ttl_article_seconds " << cache_ttl_article_seconds
				<< "\nCache_ttl_list_seconds " << cache_ttl_list_seconds
				<< "\nCache_ttl_comments_seconds " << cache_ttl_comments_seconds
				<< "\nCache_ttl_stats_seconds " << cache_ttl_stats_seconds
				<< "\nView_dedup_window_seconds " << view_dedup_window_seconds
				<< "\nView_dedup_max_per_shard " << view_dedup_max_per_shard
				<< "\nArticle_page_size_default " << article_page_size_default
				<< "\nArticle_page_size_max " << article_page_size_max
				<< "\nLogin_fail_threshold " << login_fail_threshold
				<< "\nLogin_fail_window_seconds " << login_fail_window_seconds
				<< "\nRefresh_token_ttl_seconds " << refresh_token_ttl_seconds
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
			// 记录是否显式配置过 Io_threads：旧配置只有 Sub_reactor_count，
			// 此时用它作为 io 线程数（向后兼容），但显式 Io_threads 优先。
			bool io_threads_explicit = false;

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
					// 兼容旧配置：Sub_reactor_count 曾表示"子 Reactor 数目"，
					// 现在传输层为 Asio，同一语义对应 io 线程数。
					if (!io_threads_explicit)
						io_threads = v;
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
				// ---------- 传输层（Boost.Asio / HTTP + HTTPS）----------
				else if (key == "Io_threads")
				{
					int v = 0;
					if (!parse_int(val, v) || v < 0 || v > kMaxThreadNum)
						exitAndReport("Io_threads");
					io_threads = v;
					io_threads_explicit = true;
				}
				else if (key == "Listen_address")
				{
					listen_address = val;
				}
				else if (key == "Allow_peers")
				{
					// "-" 表示"不限制"（与生成配置文件里的占位符一致）
					allow_peers = (val == "-") ? std::string() : val;
				}
				else if (key == "Http_enable")
				{
					bool v = false;
					if (!parse_bool(val, v))
						exitAndReport("Http_enable");
					http_enable = v;
				}
				else if (key == "Https_enable")
				{
					bool v = false;
					if (!parse_bool(val, v))
						exitAndReport("Https_enable");
					https_enable = v;
				}
				else if (key == "Https_port")
				{
					int v = 0;
					if (!parse_int(val, v) || v <= 0 || v > 65535)
						exitAndReport("Https_port");
					https_port = v;
				}
				else if (key == "Http_redirect_to_https")
				{
					bool v = false;
					if (!parse_bool(val, v))
						exitAndReport("Http_redirect_to_https");
					http_redirect_to_https = v;
				}
				else if (key == "Tls_cert_file")
				{
					tls_cert_file = (val == "-") ? std::string() : val;
				}
				else if (key == "Tls_key_file")
				{
					tls_key_file = (val == "-") ? std::string() : val;
				}
				else if (key == "Tls_ca_file")
				{
					tls_ca_file = (val == "-") ? std::string() : val;
				}
				else if (key == "Tls_auto_self_signed")
				{
					bool v = false;
					if (!parse_bool(val, v))
						exitAndReport("Tls_auto_self_signed");
					tls_auto_self_signed = v;
				}
				else if (key == "Tls_require_client_cert")
				{
					bool v = false;
					if (!parse_bool(val, v))
						exitAndReport("Tls_require_client_cert");
					tls_require_client_cert = v;
				}
				else if (key == "Tls_min_version")
				{
					tls_min_version = val;
				}
				else if (key == "Tls_ciphers")
				{
					tls_ciphers = (val == "-") ? std::string() : val;
				}
				else if (key == "Tls_ciphersuites")
				{
					tls_ciphersuites = (val == "-") ? std::string() : val;
				}
				else if (key == "Tls_session_tickets")
				{
					bool v = false;
					if (!parse_bool(val, v))
						exitAndReport("Tls_session_tickets");
					tls_session_tickets = v;
				}
				else if (key == "Tls_sni_certs")
				{
					tls_sni_certs = (val == "-") ? std::string() : val;
				}
				else if (key == "Body_limit_bytes")
				{
					long v = 0;
					if (!parse_long(val, v) || v <= 0)
						exitAndReport("Body_limit_bytes");
					body_limit_bytes = v;
				}
				else if (key == "Header_limit_bytes")
				{
					long v = 0;
					if (!parse_long(val, v) || v <= 0 || v > 1024L * 1024L)
						exitAndReport("Header_limit_bytes");
					header_limit_bytes = v;
				}
				else if (key == "Request_timeout_seconds")
				{
					int v = 0;
					if (!parse_int(val, v) || v <= 0)
						exitAndReport("Request_timeout_seconds");
					request_timeout_seconds = v;
				}
				else if (key == "Tls_handshake_timeout_seconds")
				{
					int v = 0;
					if (!parse_int(val, v) || v <= 0)
						exitAndReport("Tls_handshake_timeout_seconds");
					tls_handshake_timeout_seconds = v;
				}
				else if (key == "Shutdown_timeout_seconds")
				{
					int v = 0;
					if (!parse_int(val, v) || v <= 0)
						exitAndReport("Shutdown_timeout_seconds");
					shutdown_timeout_seconds = v;
				}
				else if (key == "Shutdown_drain_grace_ms")
				{
					int v = 0;
					if (!parse_int(val, v) || v <= 0)
						exitAndReport("Shutdown_drain_grace_ms");
					shutdown_drain_grace_ms = v;
				}
				// ---------- 监听器行为 ----------
				else if (key == "Listen_backlog")
				{
					int v = 0;
					if (!parse_int(val, v) || v < 0)
						exitAndReport("Listen_backlog");
					listen_backlog = v;
				}
				else if (key == "Listen_dual_stack")
				{
					bool v = false;
					if (!parse_bool(val, v))
						exitAndReport("Listen_dual_stack");
					listen_dual_stack = v;
				}
				else if (key == "Tcp_keepalive")
				{
					bool v = false;
					if (!parse_bool(val, v))
						exitAndReport("Tcp_keepalive");
					tcp_keepalive = v;
				}
				// ---------- TLS 细节 ----------
				else if (key == "Tls_alpn")
				{
					tls_alpn = (val == "-") ? std::string() : val;
				}
				else if (key == "Tls_verify_depth")
				{
					int v = 0;
					if (!parse_int(val, v) || v <= 0)
						exitAndReport("Tls_verify_depth");
					tls_verify_depth = v;
				}
				else if (key == "Tls_session_id_context")
				{
					tls_session_id_context = val;
				}
				else if (key == "Tls_session_ticket_count")
				{
					int v = 0;
					if (!parse_int(val, v) || v < 0)
						exitAndReport("Tls_session_ticket_count");
					tls_session_ticket_count = v;
				}
				// ---------- HTTP 响应与协议行为 ----------
				else if (key == "Server_header")
				{
					server_header = (val == "-") ? std::string() : val;
				}
				else if (key == "Default_content_type")
				{
					default_content_type = (val == "-") ? std::string() : val;
				}
				else if (key == "Request_id_header")
				{
					request_id_header = (val == "-") ? std::string() : val;
				}
				else if (key == "Cors_max_age")
				{
					int v = 0;
					if (!parse_int(val, v) || v < 0)
						exitAndReport("Cors_max_age");
					cors_max_age = v;
				}
				else if (key == "Cors_allow_methods")
				{
					cors_allow_methods = (val == "-") ? std::string() : val;
				}
				else if (key == "Cors_allow_headers")
				{
					cors_allow_headers = (val == "-") ? std::string() : val;
				}
				else if (key == "Cors_expose_headers")
				{
					cors_expose_headers = (val == "-") ? std::string() : val;
				}
				// ---------- 线程池 / 日志 / 指标 ----------
				else if (key == "Thread_pool_queue_size")
				{
					int v = 0;
					if (!parse_int(val, v) || v <= 0)
						exitAndReport("Thread_pool_queue_size");
					thread_pool_queue_size = v;
				}
				else if (key == "Log_async_retry_ms")
				{
					int v = 0;
					if (!parse_int(val, v) || v <= 0)
						exitAndReport("Log_async_retry_ms");
					log_async_retry_ms = v;
				}
				else if (key == "Metrics_log_every_n")
				{
					int v = 0;
					if (!parse_int(val, v) || v < 0)
						exitAndReport("Metrics_log_every_n");
					metrics_log_every_n = v;
				}
				// ---------- 健康检查快路径与依赖探活 ----------
				else if (key == "Health_fast_path")
				{
					bool v = false;
					if (!parse_bool(val, v))
						exitAndReport("Health_fast_path");
					health_fast_path = v;
				}
				else if (key == "Health_fast_path_prefixes")
				{
					health_fast_path_prefixes = (val == "-") ? std::string() : val;
				}
				else if (key == "Redis_probe_interval_seconds")
				{
					// 0 表示不做 Redis 探活（只按 enabled() 降级处理）
					int v = 0;
					if (!parse_int(val, v) || v < 0)
						exitAndReport("Redis_probe_interval_seconds");
					redis_probe_interval_seconds = v;
				}
				else if (key == "Dependency_probe_fail_threshold")
				{
					int v = 0;
					if (!parse_int(val, v) || v <= 0)
						exitAndReport("Dependency_probe_fail_threshold");
					dependency_probe_fail_threshold = v;
				}
				// ---------- MySQL 连接池 ----------
				else if (key == "DB_connect_timeout_seconds")
				{
					int v = 0;
					if (!parse_int(val, v) || v <= 0)
						exitAndReport("DB_connect_timeout_seconds");
					db_connect_timeout_seconds = v;
				}
				else if (key == "DB_max_attempts")
				{
					int v = 0;
					if (!parse_int(val, v) || v <= 0)
						exitAndReport("DB_max_attempts");
					db_max_attempts = v;
				}
				else if (key == "DB_retry_backoff_max_ms")
				{
					int v = 0;
					if (!parse_int(val, v) || v <= 0)
						exitAndReport("DB_retry_backoff_max_ms");
					db_retry_backoff_max_ms = v;
				}
				// ---------- Redis / 缓存 / 去重 / 限流 ----------
				else if (key == "Redis_uri")
				{
					redis_uri = val;
				}
				else if (key == "Redis_pool_size")
				{
					int v = 0;
					if (!parse_int(val, v) || v <= 0)
						exitAndReport("Redis_pool_size");
					redis_pool_size = v;
				}
				else if (key == "Cache_ttl_article_seconds")
				{
					int v = 0;
					if (!parse_int(val, v) || v <= 0)
						exitAndReport("Cache_ttl_article_seconds");
					cache_ttl_article_seconds = v;
				}
				else if (key == "Cache_ttl_list_seconds")
				{
					int v = 0;
					if (!parse_int(val, v) || v <= 0)
						exitAndReport("Cache_ttl_list_seconds");
					cache_ttl_list_seconds = v;
				}
				else if (key == "Cache_ttl_comments_seconds")
				{
					int v = 0;
					if (!parse_int(val, v) || v <= 0)
						exitAndReport("Cache_ttl_comments_seconds");
					cache_ttl_comments_seconds = v;
				}
				else if (key == "Cache_ttl_stats_seconds")
				{
					int v = 0;
					if (!parse_int(val, v) || v <= 0)
						exitAndReport("Cache_ttl_stats_seconds");
					cache_ttl_stats_seconds = v;
				}
				else if (key == "View_dedup_window_seconds")
				{
					int v = 0;
					if (!parse_int(val, v) || v <= 0)
						exitAndReport("View_dedup_window_seconds");
					view_dedup_window_seconds = v;
				}
				else if (key == "View_dedup_max_per_shard")
				{
					int v = 0;
					if (!parse_int(val, v) || v <= 0)
						exitAndReport("View_dedup_max_per_shard");
					view_dedup_max_per_shard = v;
				}
				else if (key == "Article_page_size_default")
				{
					int v = 0;
					if (!parse_int(val, v) || v <= 0)
						exitAndReport("Article_page_size_default");
					article_page_size_default = v;
				}
				else if (key == "Article_page_size_max")
				{
					int v = 0;
					if (!parse_int(val, v) || v <= 0)
						exitAndReport("Article_page_size_max");
					article_page_size_max = v;
				}
				else if (key == "Login_fail_threshold")
				{
					int v = 0;
					if (!parse_int(val, v) || v <= 0)
						exitAndReport("Login_fail_threshold");
					login_fail_threshold = v;
				}
				else if (key == "Login_fail_window_seconds")
				{
					int v = 0;
					if (!parse_int(val, v) || v <= 0)
						exitAndReport("Login_fail_window_seconds");
					login_fail_window_seconds = v;
				}
				else if (key == "Refresh_token_ttl_seconds")
				{
					long v = 0;
					if (!parse_long(val, v) || v <= 0)
						exitAndReport("Refresh_token_ttl_seconds");
					refresh_token_ttl_seconds = v;
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