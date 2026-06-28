# TinyServer

实现一个基于 C++11 和 POSIX 接口的轻量级并发网络服务器。

## 项目概览

### 技术栈
- **语言与标准**：C++（基于 pthread 与 POSIX 网络/IO）
- **网络模型**：epoll Reactor（主/从 Reactor），eventfd 唤醒
- **并发模型**：自定义线程池（任务队列 + 工作者线程）
- **协议与解析**：Boost.Beast 处理 HTTP 请求/响应
- **数据存储**：MySQL（连接池 + 预处理语句）
- **日志**：同步/异步日志（后台写线程 + 线程安全队列）
- **配置**：命令行参数 + 配置文件读取

---

## 目录结构 & 文件概览

核心源码均位于 `TinyServer/` 目录下：

- `main.cpp`：程序入口，注册信号处理并启动 `Server`
- `server.hpp/.cpp`：服务器主控，整合各组件
- `acceptor.hpp/.cpp`：监听端口与接收连接
- `reactor.hpp/.cpp`：epoll Reactor + wakeup(eventfd) 退出机制
- `threadpool.hpp/.cpp`：线程池（任务队列 + 工作者线程）
- `connectionpool.hpp/.cpp`：MySQL 连接池（带销毁/并发保护）
- `log.hpp/.cpp`：同步/异步日志（异步写线程 + 队列）
- `http.hpp/.cpp`：基于 Boost.Beast 的 HTTP 解析与响应序列化
- `router.hpp/.cpp`：轻量级 HTTP 路由系统
- `connection.hpp`：连接上下文封装，维护单次 TCP 会话状态
- `config.hpp/.cpp`：命令行参数与配置文件读取
- `mutex.hpp`：pthread 同步原语封装（`Mutex`/`Sem`/`CondVar`/`LockGuard`）
- `thread.hpp`：pthread 线程封装（替代 `std::thread`）
- `threadsafe_queue.hpp`：线程安全队列（用于日志异步队列）
- `error.hpp`：错误码与异常类定义

---

## 错误码规范

- **-1**：非正常退出（`defaultType`）
- **1**：数据库相关问题（`Sql_init` / `Sql_conn`）
- **2**：Reactor 模型初始化错误（`Reactor_init`）
- **3**：Acceptor 连接初始化错误（`Acceptor_init`）

---

## 核心组件说明

### 基础支持组件
- **`mutex.hpp`**：封装 `pthread_mutex_t`、`pthread_sem_t`、`pthread_cond_t`，提供 `Mutex`、`Sem`、`CondVar` 及 RAII `LockGuard`。
- **`thread.hpp`**：`Thread` 类底层封装 `pthread_create/join/detach`，捕获异常避免跨 pthread 传播。
- **`threadsafe_queue.hpp`**：线程安全队列 `ThreadSafeQueue`，底层使用 `std::queue` + `Mutex/CondVar`，提供超时阻塞 `popWithTime`。
- **`config.hpp`**：`Config` 类单例，负责解析命令行和读取默认配置文件（`./Cfg/config`），涵盖并发数（默认100线程池/150连接池）、各缓冲参数及数据库配置（JWT 密钥可配置，建议上线修改不使用默认值 `change_me`）。

### 核心业务组件
- **`log.hpp` (Log)**：全局单例日志系统。支持同步和异步模式。异步模式通过内部维护 `ThreadSafeQueue`，利用后台写线程 `worker_func_` 持续落盘避免阻塞业务层，提供宏定义如 `LOG_INFO`, `LOG_ERR` 快速输出。
- **`connectionpool.hpp` (ConnPool)**：全局单例 MySQL 连接池。封装 `getConnection()` (信号量控制) 和 `releaseConnection()` 返回池内。提供幂等且并发安全的 `destroy()` 机制等待借出的连接安稳释放。
- **`threadpool.hpp` (ThreadPool)**：指定数量工作线程，将 `SubReactor` 中唤起的请求通过 `enqueue()` 添加为闭包任务投入任务队列，任务队列满时会自动阻挡投递。
- **`http.hpp` & `router.hpp` (HTTP路由)**：
  - HTTP 利用 `Boost.Beast` 的 `http::request` 与 `http::response` 进行底层序列与反序列解析。
  - `Router` 类按精确路径和 Method 对 `std::vector<char>` 二进制明文数据分析，路由派发后转化为响应二进制输出。如：`/api/login` 返回 JWT 及 RefreshToken 等。

### 网络模型组件
- **`acceptor.hpp` (Acceptor)**：监听 TCP 端口并进行新连接请求 `accept()` 接受分配新的 File Descriptor (fd)。
- **`reactor.hpp` (Main/Sub Reactor)**：
  - `MainReactor` 运行于主线程，绑定 `Acceptor` 等待建立连接并负载给下一个 `SubReactor`。
  - `SubReactor` 每个占用一 `Thread` 跑 `loop()`，专注于自己字典中的 fd 的后续 IO 事件（`epoll_wait` 异步可读），发现可读立刻上报给 `ThreadPool`。两者均采用 `eventfd` 结构唤醒自身优雅退出。
- **`connection.hpp` (Connection)**：TCP 上下文类，用 `shared_ptr` 管理。绑定 fd，自带读写环形/流式缓冲区防止粘包半包，提供给业务存放其 `HttpRequest`/`HttpResponse` 回调处理使用。
- **`server.hpp` (Server)**：汇总控制上述所有组件。

---

## 依赖关系与执行流程

### 依赖关系图谱
1. `Server` -> `Config`, `Acceptor`, `MainReactor`, `SubReactor`, `ThreadPool`, `ConnPool` (隐式), `Log` (隐式), `Router`
2. `MainReactor` -> `Acceptor`, `SubReactor[]`
3. `SubReactor` -> 闭包中的 `std::shared_ptr<Connection>` -> `ThreadPool` 投递任务
4. `Log` & `ConnPool` 随时贯穿至单例或各类函数中调用使用。

### 事件驱动核心执行过程
1. **启动阶段**：
   - 入口为 `main()`，实例化并初始化 `Server` 对象。
   - `Server::init()` 加载配置，启动基础组件(`ConnPool` / `Log`)，配置路由并绑到 `Router`。创建主次 `Reactor`。
   - `Server::start()` 让每个 `SubReactor` 获取线程独立跑 `loop()`，主线程跑 `MainReactor::loop()`。

2. **连接到来**：
   - 客户端建立新 TCP，唤醒 `MainReactor`，调用底层 `Acceptor::accept()` 生成 `client_fd`。
   - 取模轮询放入其中一个 `SubReactor` 并调用 `SubReactor::add_fd`。
   - 为确保生命周期，`Server` 创建 `std::shared_ptr<Connection>` 对 `client_fd` 托管，向 `SubReactor` 挂载 `handle_read`/`handle_write` 闭包回调注册至 `epoll` 内。

3. **数据读写流转**：
   - 某子线程内的 `SubReactor` 内 `epoll_wait` 命中 `client_fd` 可读，响应读回调执行 `Server::handle_read(conn)`。
   - 缓冲区吸取所有字节送入 `conn->append_read_data()` 里避免阻塞 `Reactor`，并立马通过包装 Lambda 将数据交给 `Router` 路由分发进入 `ThreadPool::enqueue()` 待业程消费处理并最终序列化回应。

4. **清理机制**：
   - 若 recv 得到 0 字节内容或异常错误，闭包返回 `false`。
   - `SubReactor` 接获 `false` 时执行移除 `remove_fd` 进行关闭和内部清除维护的字典表。
   - 清除结束使得引用的 `std::function` 被销毁引致其内的 `std::shared_ptr<Connection>` 归零析构完成清理内存。
