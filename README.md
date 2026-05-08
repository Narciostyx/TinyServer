- 实现轻量化服务器

namespace project;
- **项目命名空间**
    -  包含一匿名空间，保存某些常量
    - kSleepTime，默认线程休眠时间

TinyServer
- **目录概览**（核心源码均位于`TinyServer/`）
    - `main.cpp`：程序入口，注册信号处理并启动`Server`
    - `server.hpp/.cpp`：服务器主控，整合各组件
    - `acceptor.hpp/.cpp`：监听端口与接收连接
    - `reactor.hpp/.cpp`：epoll Reactor + wakeup(eventfd) 退出机制
    - `threadpool.hpp/.cpp`：线程池（任务队列 + 工作者线程）
    - `connectionpool.hpp/.cpp`：MySQL 连接池（带销毁/并发保护）
    - `log.hpp/.cpp`：同步/异步日志（异步写线程 + 队列）
    - `http.hpp/.cpp`：极简 HTTP 解析与响应序列化
    - `config.hpp/.cpp`：命令行参数与配置文件读取
    - `mutex.hpp`：pthread 同步原语封装（`Mutex`/`Sem`/`CondVar`/`LockGuard`）
    - `thread.hpp`：pthread 线程封装（`Thread`，替代`std::thread/jthread`）
    - `threadsafe_queue.hpp`：线程安全队列（用于日志异步队列）
    - `error.hpp`：错误码与异常类

Error Code
- *错误码*
    - -1代表非正常退出
    - 1代表数据库相关问题
    - 2代表Reactor模型初始化错误
    - 3代表Acceptor连接初始化错误

Error Type（`project::kErrType`）
- *错误类型枚举*
    - `defaultType = -1`
    - `Sql_init`：MySQL 句柄/资源初始化失败
    - `Sql_conn`：MySQL 连接失败
    - `Reactor_init`：epoll/eventfd 初始化失败

# 前置依赖

## mutex.hpp
- class Mutex;
    - `lock`与`unlock`
    - `get`返回底层指向`pthread_mutex_t`的指针
    - 封装`pthread_mutex_t`
- class Sem;
    - `post`与`wait`
    - 封装`pthread_sem_t`
- class CondVar;
    - `wait`与`timedwait`
    - `signal`与`broadcast`
    - 封装`pthread_cond_t`
- class LockGuard;
    - 实现对`Mutex`类的RAII封装

## thread.hpp
- class Thread
    - **线程封装**（基于`pthread_create/join/detach`）
    - `start(Func, Args...)`：启动线程
    - `joinable` / `join` / `detach`
    - 说明：为避免跨 pthread 边界传播异常，线程入口吞掉所有异常

## alloc.hpp
- class CustomizedAllocator;
    - **自定义分配器**，线性分配器
    - **废弃**

## config.hpp
- 由于涉及文件权限等，可能会直接使程序退出（无异常处理）
- class Config;
    - **命名行参数解析与配置**

    ### 配置参数
    - `parseArg`解析命名行参数
    - `port`监听端口（默认`8080`）
    - `log_type`日志类型（`0`同步，`1`异步）
    - `thread_num`线程池内线程数量（默认`100`）
    - `sql_num`数据库连接池内连接数量（默认`150`）
    - 日志相关：`log_buffer_size`/`log_queue_size`/`log_path`/`log_row_max`/`log_row_flush`
    - 数据库相关：`address`/`dbport`/`username`/`passwd`/`dbname`/`retry`

    ### 配置来源
    - 命令行：`--port/-p`，`--logType/-l`，`--sqlNum/-s`，`--threadNum/-t`
    - 配置文件：默认路径`./Cfg/config`（不存在则创建；可从`./Cfg/config.bak`恢复）

## threadsafe_queue.hpp
- class ThreadSafeQueue;
    - **线程安全队列**
    - 使用`std::queue<std::deque<...>>`与`Mutex/CondVar`实现
    - `empty`，`full`与`size`
    - `push`与`pop`，`popWithTime`

## error.hpp
- class Err;
    - **自定义错误类**

# 组件

## main.cpp
- 程序入口
    - 注册`SIGINT`/`SIGTERM`信号处理：仅调用`Server::stop()`请求退出
    - 构造并启动`Server`

## router.hpp/router.cpp
 - class Router
     - **轻量级 HTTP 路由系统**，基于精确路径和方法进行请求分发
     - 提供单例模式，通过 `Router::get_instance()` 获取全局唯一的路由实例
     - `void register_route(const std::string& method, const std::string& path, HandlerFunc handler)`：注册一个路由，将指定的方法和路径与对应的业务处理函数（HandlerFunc）绑定
     - `std::vector<char> handle_request(const std::vector<char>& raw_request)`：对外处理总入口。接收原始字节格式的 HTTP 请求，内部反序列化出 `HttpRequest`，查找对应路由。并将产生的 `HttpResponse` 再次序列化为字节输出

## acceptor.hpp/acceptor.cpp
 - class Acceptor
     - **连接接收器类**，负责监听端口、接收新TCP连接
     - 构造函数`Acceptor(int port)`：指定端口初始化监听socket
     - 析构函数`~Acceptor()`：关闭监听socket
     - `int get_fd() const`：获取监听socket的fd
     - `int accept()`：执行一次accept，返回新连接fd

## reactor.hpp/reactor.cpp
 - class SubReactor
     - **子Reactor**：封装epoll，负责已连接fd的IO事件
     - 构造函数`SubReactor(ThreadPool* pool)`/析构函数`~SubReactor()`：创建/销毁epoll实例
     - `bool add_io_task(int fd, std::function<void(SubReactor*, int)> task)`：当fd可读时将task投递到线程池执行
     - `bool remove_fd(int)`：移除fd
     - `void loop()`：事件主循环，分发事件到回调
      - `void stop()`：通过`eventfd`唤醒`epoll_wait`并退出循环
      - `bool add_fd(int, std::function<void(int)>)`：注册fd及其事件回调

 - class MainReactor
     - **主Reactor**：封装epoll，负责监听端口，接受新连接并轮询分配给子Reactor
     - 构造函数`MainReactor(...)`/析构函数`~MainReactor()`：创建/销毁epoll实例，接受连接回调等
     - `void start()`：进入事件循环
     - `void stop()`：通过`eventfd`唤醒`epoll_wait`并退出循环
     - `void loop()`：事件主循环，分发事件到回调
      - `bool add_fd(int, std::function<void(int)>)`：注册fd及其事件回调
     - `bool remove_fd(int)`：移除fd

## server.hpp
 - class Server
     - **服务器主控类**（已实现基础整合）
     - 负责整合：`Config`、`Log`、`ConnPool`、`ThreadPool`、`Acceptor`、`MainReactor/SubReactor`
     - 主要接口：
       - `Server(int argc, char* argv[])`：解析参数并保存配置
       - `bool init()`：初始化日志/连接池/线程池/Reactor/Acceptor
       - `void start()`：启动子Reactor线程并进入主Reactor循环
       - `void stop()`：停止标志（预留优雅退出）
     - 读写生命周期控制（基于引用计数的闭包捕获）
       - `bool handle_read(std::shared_ptr<Connection> conn);`：利用智能指针维持TCP连接状态
       - `bool handle_write(std::shared_ptr<Connection> conn);`
     - 线程模型
       - `MainReactor`运行于主线程
       - 每个`SubReactor`运行于独立线程（`project::Thread`）
       - 业务处理交由`ThreadPool`工作线程执行

 ## connection.hpp
 - class Connection : public std::enable_shared_from_this<Connection>
     - **连接上下文类**，维护单个 TCP 连接的状态与数据缓冲区
     - 绑定到一个特定文件描述符 `fd`。
     - 内部包含 `read_buffer_`、`write_buffer_`，解决流式协议半包/粘包现象的基础存储对象。
     - 附带 HTTP 层级对象（`HttpRequest req` / `HttpResponse resp`），方便业务在一次连接中跨函数保留状态。

## log.hpp
- class Log
    - **日志类**，异步情况下有一个写线程并发执行
    - 全局单例
        - 首次使用需要使用`init`初始化
  	- `write`根据标志位 *is_async_* 决定调用`write_sync`与`write_async`
    - 提供宏函数`LOG_INFO`,`LOG_WARN`,`LOG_ERR`与`LOG_UNEXPECT`
    - 提供初始化日志函数logInit
    - 异步模式
        - 内部维护`ThreadSafeQueue<std::string>`
        - `write_async`将日志压入队列
        - `worker_func_`后台线程持续消费队列并写文件
        - 析构时会`join`写线程并清空队列

## connectionpool.hpp
- class ConnPool
    - **连接池类**
    - 全局单例
        - 首次使用应使用`init`初始化
    - 连接获取/归还
        - `MYSQL* getConnection()`：从池中获取连接（信号量控制并发）
        - `releaseConnection(MYSQL*)`：归还连接
    - 销毁
        - `destroy()`：线程安全、幂等；等待借出的连接归还后释放资源

## threadpool.hpp
- class ThreadPool
    - **线程池类**，用于管理和复用多个工作线程，处理异步任务
    - 构造函数`ThreadPool(size_t thread_count = kDefaultThreadNum, size_t max_requests = kDefaultRequestNum)`：
      - 初始化线程池并启动指定数量的线程
      - 增加最大请求数控制（任务队列满时`enqueue`阻塞等待）
    - 析构函数`~ThreadPool()`：安全关闭线程池
    - `enqueue(std::function<void()>)`：向线程池添加一个任务，任务为可调用对象
    - 内部维护：
        - `std::vector<Thread> workers_`：工作线程集合
        - `std::queue<std::function<void()>> tasks_`：任务队列
        - `Mutex queue_mutex_`：任务队列互斥锁
        - `CondVar condition_`：条件变量用于线程同步
        - `CondVar not_full_`：队列满时用于阻塞等待
        - `std::atomic<bool> stop_`：停止标志
        - `size_t max_threads_`：最大线程数
        - `size_t max_requests_`：最大请求数
        - `void worker()`：工作线程主函数

## http.hpp/http.cpp
- 极简 HTTP 支持
    - `HttpRequest`：method/path/version/headers/body
    - `HttpResponse`：status/reason/headers/body，`to_string()`序列化
    - `parse_http_request()`：解析请求（最简，支持`Content-Length`）

# 依赖关系 & 执行执行流程

## 核心依赖流
1. `Server` -> `Config`, `Acceptor`, `MainReactor`, `SubReactor`, `ThreadPool`, `ConnPool` (隐式单例), `Log` (隐式单例), `Router` (单例)
2. `MainReactor` -> `Acceptor`, `SubReactor[]`
3. `SubReactor` -> 闭包中的 `std::shared_ptr<Connection>` -> 线程池 (`ThreadPool`) 投递任务
4. `Log` & `ConnPool` 为基础组件，贯穿在任意函数的生命周期中被调用。

## 事件驱动核心执行过程
1. **启动阶段**：
   - 入口为 `main()`，实例化并初始化 `Server` 对象。
   - `Server::init()` 加载配置，启动基础组件(`ConnPool` / `Log`)，随后配置路由并将相关操作绑定到 `Router`。创建主次`Reactor`并设定挂载逻辑。
   - `Server::start()` 让每个 `SubReactor` 获取并挂接专门的 `Thread` 执行 `loop()`。主线程开始执行 `MainReactor::loop()`。
2. **连接到来**：
   - 客户端连接请求到达（触发 `listen fd` 上的可读事件）。
   - `MainReactor` 被唤醒，调用底层 `Acceptor::accept()` 生成一个新的客户端通讯套接字 (`client_fd`)。
   - `MainReactor` 按轮询方式选中一个 `SubReactor`，将 `client_fd` 通过 `SubReactor::add_fd` 派发过去。
   - 这里利用高阶函数捕获闭包对象：`Server` 创建了 `std::shared_ptr<Connection>`，将带有 `conn` 的 `handle_read`/`handle_write` 闭包（`std::function`）注册到 `SubReactor` 内部的 `FdContext`。这使得生命周期托管给 `SubReactor` 底层。
3. **数据读写流转**：
   - 处于某子线程的 `SubReactor::loop()` 通过 `epoll_wait` 发现该 `client_fd` 有可读事件。
   - `SubReactor` 使用自身绑定的读取回调（实际上执行的是 `Server::handle_read(conn)`）。
   - `handle_read` 使用局部变量作为栈缓冲区收集 `recv()` 的数据，并全部压装到 `conn->append_read_data()` 中。
   - 这避免了阻塞 `Reactor`，`Server` 紧接着将数据交于 `Router`，并将业务逻辑路由与回复数据 `send` 等阻塞操作，封装成 `Lambda` 包装后，推送到 `ThreadPool::enqueue()`。
4. **清理机制**：
   - 当收到 0 字节长（客户端关闭）或发生严重读错误，`handle_read`/`handle_write` 闭包回调会返回 `false`。
   - `SubReactor` 检测到返回结果后，会主动调用 `remove_fd`，关闭套接字，同时清除在字典里的对应记录。
   - 此时 `std::function` 会被销毁，伴带生命周期托管的 `std::shared_ptr<Connection>` 的引用计数归零，内存空间安全回收。
