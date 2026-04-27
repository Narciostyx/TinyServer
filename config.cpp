#include "config.hpp"

namespace project
{

	void Config::parseArg(int argc, char* argv[])
	{
		int opt = 0, long_index = 0;
		if (argc == 1)
		{
			std::cout << "Will use the default file to load the configuration.\n";
			loadConfigFromFile();
			printInfo();
			return;
		}

		loadConfigFromFile();

		while ((opt = getopt_long(argc, argv, short_option, long_option, &long_index)) != -1)
		{
			switch (opt)
			{
			case 'p':
				port = atoi(optarg);
				if (port <= 0 || port > 65535)
				{
					std::cout << "Illegal port!\n";
					exit(exit_code = -1);
				}
				break;
			case 'l':
				log_type = atoi(optarg);
				if (log_type < 0 || log_type>1)
				{
					std::cout << "The argument of logType must be 0 or 1!\n";
					exit(exit_code = -1);
				}
				break;
			case 's':
				sql_num = atoi(optarg);
				if (sql_num <= 0 || sql_num > kMaxSqlNum)
				{
					std::cout << "Ilegal value for the sql connections.\n";
					exit(exit_code = -1);
				}
				break;
			case 't':
				thread_num = atoi(optarg);
				if (thread_num <= 0 || thread_num > kMaxThreadNum)
				{
					std::cout << "Ilegal value for the threadpool.\n";
					exit(exit_code = -1);
				}
				break;
			case 'h':
			case '?':
				printHelp();
				exit(0);
				break;
			case 'v':
				printVersion();
				exit(0);
				break;
			default:
				exit(exit_code = -1);
			}
		}
		std::cout << "Arguments have been parsed:\n";
		printInfo();
	}
}