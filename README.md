# TinyServer

基于 **C++20 + Linux epoll** 的轻量级并发 HTTP 服务器：主/从 Reactor 网络模型、自定义线程池、MySQL 连接池、JWT 鉴权与 Redis 缓存/限流/去重，前端为纯静态页面。

> 仅支持 Linux（依赖 epoll / eventfd / pthread，构建期校验平台）。

## 技术栈与架构

- **语言/标准**：C++20（线程与同步一律使用标准库 `std::thread` / `std::mutex` / `std::condition_variable` / `std::counting_semaphore`）
- **网络模型**：epoll 主/从 Reactor（**边缘触发 ET + 非阻塞 fd**），eventfd 跨线程唤醒
- **并发模型**：线程池（`std::thread` + 有界任务队列，队列满快速失败做背压保护，不阻塞事件循环）
- **HTTP**：Boost.Beast 解析/序列化；解析三态 `Ok/NeedMore/Error`，半包缓冲累积等待补齐
- **存储**：MySQL 连接池（信号量控制 + 预处理语句防注入）
- **缓存/会话**：Redis（redis-plus-plus + hiredis，默认依赖）——详情/列表/评论/统计缓存、浏览量去重、登录限流、refresh token 吊销
- **鉴权**：JWT（libjwt, HS256）双 token（access/refresh）
- **日志**：同步/异步日志（`ILogger` 抽象 + 后台写线程 + 线程安全队列）
- **可观测**：Prometheus 风格 `/metrics`，`/healthz/live`、`/healthz/ready` 探针
- **前端**：`Web/` 纯静态页（登录/注册、列表、详情、发布，现代内容站风格）

## 目录结构

核心源码在 `TinyServer/`：

| 文件 | 职责 |
|---|---|
| `main.cpp` | 入口：信号处理（SIGINT/SIGTERM → 优雅停机） |
| `server.hpp/.cpp` | `Server` 主控：组装组件、定义回调、生命周期管理 |
| `acceptor.hpp/.cpp` | 监听/accept，accept 后 fd 设非阻塞并限 127.0.0.1 |
| `reactor.hpp/.cpp` | `MainReactor`（主线程 accept+分发）/ `SubReactor`（每线程一台 epoll）：post 任务队列、最小堆空闲超时、`flush_write` |
| `connection.hpp` | 连接会话容器：读写缓冲（内部锁）、任务在飞标志、owner 指针（连接级串行化） |
| `threadpool.hpp/.cpp` | 线程池：`enqueue`（背压阻塞）/ `try_enqueue`（Reactor 侧非阻塞投递） |
| `connectionpool.hpp/.cpp` | MySQL 连接池 + `stmt_rw_execute`（预处理模板，含事务借出 `borrow/giveBack`） |
| `service.hpp/.cpp` | `DataService`：全部 SQL 走预处理语句；缓存写失效接入 |
| `router.hpp/.cpp` | 路由分发 + JWT 签发/校验 + 输入校验（UTF-8 字符计数）+ 接口实现 |
| `redis_store.hpp/.cpp` | Redis 统一封装（默认依赖；运行期连不上自动降级） |
| `http.hpp/.cpp` | Beast 解析（body_limit 64KB）/ 序列化 |
| `log.hpp/.cpp` | 同步/异步日志（`ILogger` + 默认适配器），`LOG_*` 宏 |
| `metrics.hpp` | 指标注册表 + Prometheus/健康检查渲染 |
| `threadsafe_queue.hpp` | 线程安全队列（std 实现，供日志异步队列） |
| `config.hpp/.cpp` | 命令行 + `./Cfg/config` 配置（自动生成/回退） |
| `error.hpp` | 异常 `Err` + `kErrType` + 退出码 |
| `thread.hpp` / `mutex.hpp` | **已弃用（`[[deprecated]]`）**：早期 pthread 封装，保留兼容；勿用于新代码 |

## 依赖与构建（Linux）

| 依赖 | 说明 | Ubuntu 安装 |
|---|---|---|
| CMake ≥ 3.16 / GCC ≥ 11 | C++20 + `<semaphore>` | `sudo apt install build-essential cmake g++` |
| Boost（Beast header-only / json 库） | HTTP 解析 | `sudo apt install libboost-dev libboost-json-dev` |
| MySQL client | 连接池 | `sudo apt install libmysqlclient-dev` |
| libjwt | HS256 JWT | `sudo apt install libjwt-dev` |
| hiredis | redis++ 底层 | `sudo apt install libhiredis-dev redis-server` |
| **redis-plus-plus** | 无发行版，需源码编译（产物 `libredis++.so`） | 见下 |

```bash
# redis-plus-plus（无官方 release，必须自行编译安装）
git clone https://github.com/sewenew/redis-plus-plus
cd redis-plus-plus && mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DREDIS_PLUS_PLUS_CXX_STANDARD=20 -DREDIS_PLUS_PLUS_BUILD_TEST=OFF
make -j && sudo make install     # 头文件 /usr/local/include，库 /usr/local/lib/libredis++.so

# 构建本服务（Redis 为默认依赖，无需开关）
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

> 库找不到时 CMake 会 FATAL_ERROR 并打印安装指引；运行时链接需求（与 `readelf -d` 对应）：`libmysqlclient`、`libboost_json`、`libjwt`、`libredis++.so`。

## 运行与配置

```bash
# 环境变量（均可选）
TINYSERVER_REDIS_URI=tcp://127.0.0.1:6379   # Redis 地址，默认即本机 6379，可省略
TINYSERVER_CORS_ORIGIN=https://example.com  # CORS 白名单，默认 *

./build/TinyServer -p 8080   # 命令行参数见 -h；其余配置读 ./TinyServerVar/config（不存在则自动生成）
```

- Redis **连不上不致命**：服务照常启动，日志给 WARN；业务层 `enabled()` 为 false 后自动降级（缓存穿透 DB、限流/去重回退进程内）。
- 数据库需按 `TinyServer/DB 结构`（user/article/comment/user_likes 四表，见下方 DDL 注）预建 `webdatabase`，默认账号 `webdb/webdb`（可在配置文件修改）。
- JWT 密钥须 ≥32 字符：内置演示默认值，**上线必须修改**（改 `Cfg/config` 的 `Jwt_secret`）。

## 核心机制速览

- **主从 Reactor**：主线程只 accept 并用 `post()` 把新连接"过户"到某个 SubReactor（`fd_contexts_` 仅在子线程访问，无锁）；SubReactor 拥有连接的收发/超时/关闭。
- **连接级串行化**：同一连接同一时刻至多一个业务任务（`task_in_flight` 原子令牌），`req/resp` 因而可复用；worker 只产字节，发送/关闭统一 `post` 回 reactor 线程执行（消除 fd 重用竞争）。
- **ET + 半包**：循环 `recv` 到 EAGAIN；HTTP 解析三态，`NeedMore` 放回缓冲等补齐。
- **空闲超时**：最小堆 + lazy deletion + 1s 节流，`epoll_wait` 超时与下一到期时刻联动。
- **线程池背压**：Reactor 侧用 `try_enqueue`，队列满直接关闭连接（快速失败）避免拖死事件循环。
- **Redis**：cache-aside（文章详情/列表/评论/统计）、`SETNX` 浏览量去重、登录失败限流、refresh token 黑名单。键表与失效策略见 `REDIS_INTEGRATION.md`。
- **缓存一致性**：写路径先写 MySQL 再删缓存；短 TTL 弱一致兜底。

## 事件驱动执行过程

1. **启动**：`main` → `Server::init`（日志 → JWT 强度校验 → Redis 连接 → MySQL 连接池 → 线程池 → SubReactor[] → Acceptor → MainReactor）→ `start`（各 SubReactor 起线程跑 `loop`，主线程进 `MainReactor::loop`）。
2. **连接到达**：`MainReactor` 命中监听 fd → `Acceptor::accept`（非阻塞）→ 轮询选 SubReactor → `post(add_fd + set_callbacks + Connection 领养)`。
3. **请求处理**：SubReactor `EPOLLIN` → `handle_read` 循环收进读缓冲 → `try_enqueue(process_request)` → worker 解析/路由/查库/序列化到写缓冲 → `post` 回 reactor → `flush_write`（EAGAIN 转 EPOLLOUT 续发）→ keep-alive 或关闭。
4. **清理/停机**：断连/空闲超时 → reactor 线程 `remove_fd`（幂等）；信号 → `Server::stop`（幂等）唤醒各 reactor 退出 → join → 析构先收线程池。

## 错误码规范

- **-1**：配置/未知非正常退出
- **1**：数据库初始化/连接（`Sql_init`/`Sql_conn`）
- **2**：Reactor 初始化（`Reactor_init`）
- **3**：Acceptor 绑定/监听失败（`Acceptor_init`）

> `kErrType` 另含 `Thread_wrong`、`Redis_error`（Redis 运行期降级按 WARN 处理，不触发进程退出）。

## 文档索引

- 接口约定：`API_CONVENTIONS.md`（REST 风格 + 注册/登录/文章/评论约定）


## 说明

- 数据库 DDL（`webdatabase` 的 article/comment/user/user_likes 表结构）以项目实际使用的建表语句为准（utf8mb4、`user.username` 唯一、`user_likes` 唯一键 `(user_id, article_id)` 等），见 `INTERVIEW_PREP.md` 或本地建表脚本。
