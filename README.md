# TinyServer

基于 **C++20 + Boost.Asio** 的轻量级并发 HTTP/HTTPS 服务器：**Asio（io_context + Asio SSL）事件驱动传输层**、自定义线程池、MySQL 连接池、JWT 鉴权与 Redis 缓存/限流/去重，前端为纯静态页面。

> 仅支持 Linux（依赖 epoll / eventfd / pthread，构建期校验平台）。
>
> **架构变更（2026）**：网络层已从"主/从 Reactor + 手写 epoll"迁移到 **Boost.Asio**，
> 并在此基础上实现 **HTTPS/TLS**。旧实现在 `TinyServer/legacy/`（保留供对照，不参与编译）。

## 技术栈与架构

- **语言/标准**：C++20（线程与同步使用标准库 `std::thread` / `std::mutex` / `std::condition_variable` / `std::counting_semaphore`）
- **传输层**：**Boost.Asio `io_context`（N 个 io 线程）+ `ip::tcp::acceptor`**；每条连接一个 `HttpSession<Stream>`，socket 建立在独立 **strand** 上（回调串行 ⇒ 会话对象内部无需加锁）
- **HTTPS**：**OpenSSL + `asio::ssl::stream`**。TLS1.2 起（可配 1.3）、ALPN 声明 `http/1.1`、会话复用（session id + ticket）、可选 **mTLS**（客户端证书）、可选 **SNI 多证书**、**SIGHUP 热重载证书**
- **HTTP**：Boost.Beast 解析/序列化。`async_read_header` + `async_read` 两段式（支持 `Expect: 100-continue`）；`flat_buffer` 常驻 ⇒ 天然支持 keep-alive 与 **pipelining**
- **并发模型**：io 线程只做非阻塞收发/解析/超时；业务处理投递到 **线程池**（内部是阻塞的 MySQL/Redis 调用），完成后 post 回会话 strand 写响应
- **存储**：MySQL 连接池（信号量控制 + 预处理语句防注入）
- **缓存/会话**：Redis（redis-plus-plus + hiredis，默认依赖）——详情/列表/评论/统计缓存、浏览量去重、登录限流、refresh token 吊销
- **鉴权**：JWT（libjwt, HS256）双 token（access/refresh）
- **日志**：同步/异步日志（`ILogger` 抽象 + 后台写线程 + 线程安全队列）
- **可观测**：Prometheus 风格 `/metrics`（含**连接数、被拒连接、TLS 握手成败**）、`/healthz/live`、`/healthz/ready` 探针
- **运维**：`SIGINT/SIGTERM` 优雅停机（停止 accept → 会话收尾 → 排空 → 退出，第二次信号强制退出）；`SIGHUP` 重载证书
- **前端**：`Web/` 纯静态页（登录/注册、列表、详情、发布，现代内容站风格）

## 目录结构

核心源码在 `TinyServer/`：

| 文件 | 职责 |
|---|---|
| `main.cpp` | 入口：忽略 SIGPIPE（TLS 写失败不能杀进程）；信号由传输层的 `signal_set` 处理 |
| `server.hpp/.cpp` | `Server` 主控（组合根）：组装组件、定义业务回调、映射配置、生命周期与停机顺序 |
| `asio_transport.hpp/.cpp` | **Asio 传输层**：`io_context`/io 线程、HTTP+HTTPS 监听、TLS 上下文（ALPN/mTLS/SNI/会话复用）、CIDR 白名单、`signal_set`、停机排空 |
| `http_session.hpp` | **`HttpSession<Stream>`**：**C++20 协程**驱动的单连接处理流程（握手 → 读头 → 100-continue → 读体 → 投递业务 → 写响应 → keep-alive 循环 → 优雅关闭）；空闲/请求体/握手/关闭四类看门狗超时；413/431/408/503；HEAD 正确语义 |
| `http.hpp/.cpp` | HTTP 类型别名与限额、`Date`/`Server` 默认头、统一 JSON 错误体 |
| `threadpool.hpp/.cpp` | 业务线程池：`try_enqueue`（io 线程非阻塞投递，满则回 503）、`enqueue`（阻塞式） |
| `connectionpool.hpp/.cpp` | MySQL 连接池 + `stmt_rw_execute`（预处理模板，含事务借出 `borrow/giveBack`） |
| `service.hpp/.cpp` | `DataService`：全部 SQL 走预处理语句；缓存写失效接入 |
| `router.hpp/.cpp` | 路由分发 + JWT 签发/校验 + 输入校验（UTF-8 字符计数）+ 接口实现 |
| `redis_store.hpp/.cpp` | Redis 统一封装（默认依赖；运行期连不上自动降级） |
| `log.hpp/.cpp` | 同步/异步日志（`ILogger` + 默认适配器），`LOG_*` 宏 |
| `metrics.hpp` | 指标注册表 + Prometheus/健康检查渲染 |
| `threadsafe_queue.hpp` | 线程安全队列（供日志异步队列） |
| `config.hpp/.cpp` | 命令行 + `./TinyServerVar/config` 配置（自动生成/回退） |
| `error.hpp` | 异常 `Err` + `kErrType` + 退出码 |
| `legacy/` | **旧 epoll 主从 Reactor 实现**（`reactor`/`connection`/`acceptor`），已被取代，不参与编译 |
| `thread.hpp` / `mutex.hpp` / `alloc.hpp` | **已弃用（`[[deprecated]]`）**：早期 pthread 封装，保留兼容；勿用于新代码 |

## 依赖与构建（Linux）

| 依赖 | 说明 | Ubuntu 安装 |
|---|---|---|
| CMake ≥ 3.16 / GCC ≥ 11 | C++20 + `<semaphore>` | `sudo apt install build-essential cmake g++` |
| Boost（Asio + Beast 头文件 / json 库） | 传输层与 HTTP 解析 | `sudo apt install libboost-dev libboost-json-dev` |
| **OpenSSL** | **HTTPS/TLS** | `sudo apt install libssl-dev` |
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

> **关于单元测试目标**：`test_*.cpp` 目前内容整体被注释（没有 `main`），因此
> `TINYSERVER_BUILD_TESTS` **默认为 OFF**；补齐真实用例后再用 `-DTINYSERVER_BUILD_TESTS=ON` 打开。
> 这四个文件各自带 `main`，**不要**把它们加进主可执行文件的源列表，否则会与 `main.cpp` 冲突
> （`multiple definition of 'main'`）；`TinyServer.vcxproj` 里它们已从 `ClCompile` 移到 `None`。

> 库找不到时 CMake 会 FATAL_ERROR 并打印安装指引；运行时链接需求（与 `readelf -d` 对应）：
> `libmysqlclient`、`libboost_json`、`libjwt`、`libredis++.so`、**`libssl` / `libcrypto`**。
>
> Boost.Asio / Boost.Beast 为头文件库，无需额外链接（Boost ≥ 1.69 起 `Boost.System` 亦已头文件化；
> 若发行版 Boost 更旧，请在 CMakeLists 的 `COMPONENTS` 里追加 `system`）。

## 运行与配置

### 快速体验 HTTPS

**HTTPS 默认开启**（端口 8443），证书缺失时会自动生成自签证书，因此直接启动即可：

```bash
./build/TinyServer                               # 同时监听 :8080(HTTP) 与 :8443(HTTPS)
curl -k https://127.0.0.1:8443/api/ping          # -> pong
curl -k https://127.0.0.1:8443/metrics           # 含 tinyserver_tls_handshakes_total
```

启动日志会给出 `已自动生成自签证书（仅用于开发/测试）…` 的 WARN。
**自签证书仅供本地开发**：浏览器会告警、客户端需 `-k` 或显式信任；生产请把 `Tls_auto_self_signed`
设为 `0` 并配置受信任证书（否则相当于对外提供无法验证的证书，可被中间人冒充）。

若想自己生成证书，或已有受信任证书：

```bash
mkdir -p TinyServerVar/tls
openssl req -x509 -newkey rsa:2048 -nodes -days 365 \
    -subj "/CN=localhost" \
    -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" \
    -keyout TinyServerVar/tls/server.key -out TinyServerVar/tls/server.crt

# 命令行可覆盖：-S 开启 HTTPS，-P 端口，-C 证书，-K 私钥
./build/TinyServer -p 8080 -S -P 8443 -C TinyServerVar/tls/server.crt -K TinyServerVar/tls/server.key
```

> ⚠️ **已存在 `TinyServerVar/config` 的情况**：配置文件的值优先于代码默认值，旧文件里的
> `Https_enable 0` 会让 HTTPS 保持关闭。想采用新的默认行为，请把它改成 `1`（并补上
> `Tls_auto_self_signed 1`），或删除该文件让程序重新生成。

一键验收（TLS 版本、ALPN、413/431、100-continue、pipelining、HEAD、快路径抗饱和、优雅停机等逐项打勾）：

```bash
BIN=./build/TinyServer ./scripts/verify_https.sh
```

### 环境变量（均可选）

```bash
TINYSERVER_REDIS_URI=tcp://127.0.0.1:6379   # Redis 地址，默认即本机 6379，可省略
TINYSERVER_CORS_ORIGIN=https://example.com  # CORS 白名单，默认 *
```

### 配置文件 `./TinyServerVar/config`

命令行仅覆盖常用项，其余配置在 `./TinyServerVar/config`（不存在则自动生成）。传输层相关键：

| 键 | 默认值 | 说明 |
|---|---|---|
| `Listen_address` | `127.0.0.1` | 监听网卡地址；`0.0.0.0` / `::` 表示所有网卡 |
| `Allow_peers` | `127.0.0.1/32,::1/128` | 逗号分隔的 **IP/CIDR 白名单**；空值表示不限制 |
| `Io_threads` | `0` | io 线程数，`0` = 自动（`max(2, CPU 核数)`） |
| `Http_enable` / `Port` | `1` / `8080` | 明文 HTTP 监听开关与端口 |
| `Https_enable` / `Https_port` | **`1`** / `8443` | **HTTPS 默认开启**；置 `0` 可关闭 |
| `Http_redirect_to_https` | `0` | `1` 时明文监听器只回 **301** 跳转到 HTTPS |
| `Tls_cert_file` / `Tls_key_file` | `./TinyServerVar/tls/server.{crt,key}` | PEM 证书链与私钥 |
| `Tls_auto_self_signed` | `1` | **证书缺失时自动生成自签证书**（RSA-2048 + SAN，写入 `./TinyServerVar/tls/`，私钥权限 0600）。生产请置 `0` |
| `Tls_min_version` | `1.2` | `1.0`/`1.1`/`1.2`/`1.3` |
| `Tls_ciphers` / `Tls_ciphersuites` | 空（OpenSSL 默认） | TLS1.2- / TLS1.3 套件（`-` 表示空） |
| `Tls_ca_file` / `Tls_require_client_cert` | 空 / `0` | 配置即启用 **mTLS**；`1` 时强制要求客户端证书 |
| `Tls_session_tickets` | `1` | 会话票据（会话复用，降低握手开销） |
| `Tls_sni_certs` | 空 | SNI 多证书：`a.com:/p/a.crt:/p/a.key,b.com:/p/b.crt:/p/b.key` |
| `Tls_alpn` | `http/1.1` | 逗号分隔的 ALPN 协议名（上 HTTP/2 时追加 `h2`） |
| `Tls_verify_depth` | `4` | mTLS 客户端证书链校验深度 |
| `Tls_session_id_context` | `TinyServer` | 会话复用所需的 session id context |
| `Tls_session_ticket_count` | `2` | 服务端下发的 session ticket 数量 |
| `Body_limit_bytes` / `Header_limit_bytes` | `65536` / `8192` | 超限分别回 **413** / **431** |
| `Time_out_connection` | `600` | keep-alive 空闲超时（等待下一个请求头） |
| `Request_timeout_seconds` | `30` | 读请求体上限（Slowloris 防护） |
| `Tls_handshake_timeout_seconds` | `10` | TLS 握手上限 |
| `Shutdown_timeout_seconds` | `10` | 优雅停机排空超时 |
| `Shutdown_drain_grace_ms` | `500` | 硬停机后等待会话关闭回调跑完的宽限 |
| `Max_listening_connection` | `5000` | 单进程最大并发连接（超出直接拒绝并计入指标） |
| `Listen_backlog` | `0` | `listen()` 队列长度；`0` = 使用系统上限 SOMAXCONN |
| `Listen_dual_stack` | `1` | IPv6 监听地址是否同时接受 IPv4（v4-mapped）连接 |
| `Tcp_keepalive` | `1` | 已接受连接是否开启 TCP keepalive |

**响应头与协议行为**（原先硬编码，现均可配置；值写 `-` 表示留空/不发送该头）：

| 键 | 默认值 | 说明 |
|---|---|---|
| `Server_header` | `TinyServer` | `Server` 响应头取值 |
| `Default_content_type` | `application/json; charset=utf-8` | 未显式设置时的 `Content-Type` |
| `Request_id_header` | `X-Request-Id` | 请求追踪头：回显客户端值，缺失则生成 |
| `Cors_max_age` | `600` | 预检结果缓存秒数 |
| `Cors_allow_methods` | `GET, POST, PUT, PATCH, DELETE, OPTIONS` | 同时用于 `405` 的 `Allow` 头 |
| `Cors_allow_headers` | `Content-Type, Authorization, X-Request-Id` | |
| `Cors_expose_headers` | `X-Request-Id` | |

**线程池 / 日志 / 指标 / 数据库 / 缓存**（原先散落在各文件里的硬编码常量）：

| 键 | 默认值 | 说明 |
|---|---|---|
| `Thread_pool_queue_size` | `100` | 业务线程池队列容量（即背压阈值，满则回 503） |
| `Health_fast_path` | `1` | `/healthz`、`/metrics` 等**不经业务线程池**，在 io 线程内直接处理 |
| `Health_fast_path_prefixes` | `/healthz,/livez,/readyz,/metrics` | 快路径前缀（逗号分隔） |
| `Redis_probe_interval_seconds` | `10` | Redis 探活间隔；`0` = 不探活（只按 `enabled()` 降级） |
| `Dependency_probe_fail_threshold` | `3` | 连续失败多少次才判定依赖不可用（防抖） |
| `Log_async_retry_ms` | `10` | 异步日志队列满时的重试间隔（重试 3 次后丢弃并计数） |
| `Metrics_log_every_n` | `100` | 每 N 个响应打印一次指标快照，`0` = 关闭 |
| `DB_connect_timeout_seconds` | `5` | MySQL 连接超时 |
| `DB_max_attempts` | `10` | 连接重试上限（仅 `DB_retry=1` 时生效） |
| `DB_retry_backoff_max_ms` | `10000` | 重试退避上限（`min(1000*attempt, 该值)`） |
| `Redis_uri` | `tcp://127.0.0.1:6379` | 可被环境变量 `TINYSERVER_REDIS_URI` 覆盖 |
| `Redis_pool_size` | `8` | 预留（当前用 redis-plus-plus 默认连接池） |
| `Cache_ttl_article_seconds` | `300` | 文章详情缓存 TTL |
| `Cache_ttl_list_seconds` | `30` | 文章列表缓存 TTL |
| `Cache_ttl_comments_seconds` | `60` | 评论列表缓存 TTL |
| `Cache_ttl_stats_seconds` | `30` | 用户统计缓存 TTL |
| `View_dedup_window_seconds` | `10` | 浏览量去重窗口（Redis SETNX EX 与进程内降级共用） |
| `View_dedup_max_per_shard` | `4096` | 进程内去重表每分片容量上限（分片数 16 为编译期常量） |
| `Article_page_size_default` | `100` | 列表分页默认页大小（同时决定列表缓存失效键 `articles:list:1:N`） |
| `Article_page_size_max` | `100` | 单页上限 |
| `Login_fail_threshold` | `5` | 登录失败限流阈值 |
| `Login_fail_window_seconds` | `300` | 登录失败限流窗口 |
| `Refresh_token_ttl_seconds` | `2592000` | 登出后 refresh token 黑名单 TTL |

> 仍保留为**编译期常量**的两处（有意为之）：`ConnPool` 的信号量模板上限 `kConnPoolSemMax`、
> `ViewDedup` 的分片数 `kShards`（互斥锁数组维度）；另有 `router.hpp` 里的用户名/密码/标题/正文
> 长度校验常量——它们与数据库列定义（`varchar(255)`、`text`）绑定，改配置而不改表会直接写失败。

> 注意：配置文件按 `键 值` **逐 token** 读取，值里不能含空格（尤其是证书路径）。
> 旧的 `Sub_reactor_count` 仍可识别，会作为 `Io_threads` 的兼容取值。

其它要求：

- Redis **连不上不致命**：服务照常启动，日志给 WARN；业务层 `enabled()` 为 false 后自动降级。
- 数据库需按 `TinyServer/DB 结构`（user/article/comment/user_likes 四表）预建 `webdatabase`，默认账号 `webdb/webdb`。
- JWT 密钥须 ≥32 字符：内置演示默认值，**上线必须修改**（改 `TinyServerVar/config` 的 `Jwt_secret`）。

## 核心机制速览

- **Asio 事件驱动**：`io_context` 跑 N 个 io 线程；每条连接一个 `HttpSession`，socket 建于独立 strand ⇒
  该会话的所有回调串行执行，整条协程都在同一 strand 上推进，**成员变量无需加锁**。
- **一条连接 = 一条协程**：`HttpSession::run()` 把"握手 → 读头 → 100-continue → 读体 → 投递业务 →
  写响应 → keep-alive 循环 → 优雅关闭"写成直线代码；早先的回调状态机
  （`do_x`/`on_x`、`writing_`/`reading_` 边界判断、错误时"先写响应再关闭"的嵌套闭包）全部消失。
- **阻塞业务仍走线程池**：Router/MySQL/Redis 是阻塞调用，通过自定义 awaitable `PoolTaskAwaitable`
  把任务投递到业务线程池并挂起协程，完成后在会话 strand 上恢复；
  **池满时协程不挂起**（`await_suspend` 返回 false），调用方直接回 **503 + Retry-After**。
- **看门狗而非 per-op 取消**：空闲/请求体/握手/关闭四类定时器独立运行（不 await），
  到期即关闭 socket，挂起的读写以 `operation_aborted` 返回、协程据此退出。
  这样不依赖各版本 Boost 的 per-operation cancellation 支持。
- **连接身份 = 对象本身**：不再有"按 fd 编号查表"的逻辑，因此不存在旧实现里
  "fd 被内核复用后把旧连接的响应发给新客户端"的串包风险。
- **健康检查走快路径**：`/healthz`、`/livez`、`/readyz`、`/metrics` 命中配置前缀后**在 io 线程内直接处理**，
  不投递业务线程池。否则业务池饱和时探针会拿到 503，被编排系统误判为实例故障并重启。
  约定：被判定的端点内部不得有阻塞 IO。
- **依赖探活**：独立的探活线程（非业务池）每 200ms 采样 MySQL 池水位、并用 `try_getConnection()`
  非阻塞判定"池是否还能借到连接"；Redis 按间隔 `PING`。连续失败达阈值才置 DOWN（防抖），
  结果进入 `/metrics` 与 `/healthz/ready`（`is_ready() = 启动完成 && 依赖健康`）。
- **日志热路径**：同步模式按 `Log_row_flush` 批量 `flush`（不再每行一次系统调用）；
  异步模式队列满时在锁外重试、仍失败则丢弃并计数（不再持锁 `usleep` 忙等），
  丢弃量由 `tinyserver_log_dropped_lines_total` 暴露。
- **两段式读取**：`async_read_header` → 长度预检（超限直接 413，**不**回 100-continue）→ 需要时回
  `100 Continue` → `async_read` 读 body。半包/粘包由 Beast parser + 常驻 `flat_buffer` 处理，天然支持 pipelining。
- **HEAD 语义正确**：用 `serializer::skip(true)` 发送与 GET 相同的头（含 `Content-Length`）但不发 body，
  避免响应体污染 keep-alive 流。
- **优雅停机**：`signal_set` 收到 SIGINT/SIGTERM → 关闭 acceptor → 通知会话（空闲的立即关、在处理的写完再关）
  → 会话归零或 `Shutdown_timeout_seconds` 到期后**先硬关所有残留会话、再停事件循环**
  （保证 socket 在 io_context 存活时析构）；期间再收一次信号则强制退出。
- **TLS 热重载**：`SIGHUP` 重建 SSL_CTX；新连接用新证书，旧连接持有的旧上下文只"退役"不释放（避免 UB）。
- **配置驱动**：协议行为（超时/限额/响应头/CORS）、缓存 TTL、去重与限流参数、连接池与线程池容量
  全部来自 `Config`，不再散落为文件内硬编码常量。
- **缓存一致性**：写路径先写 MySQL 再删缓存；短 TTL 弱一致兜底。

## 事件驱动执行过程

1. **启动**：`main` → `Server::init`（日志 → JWT 强度校验 → Redis → MySQL 连接池 → 业务线程池 →
   `AsioTransport::init`：建 TLS 上下文 + 绑定监听 + 注册 `signal_set`）→ `start`（起 io 线程，
   调用线程进入 `io_context::run`）。
2. **连接到达**：acceptor 完成 → `on_accept`（CIDR 白名单与连接数上限检查）→ 建 `HttpSession`（TLS 则先 `async_handshake`）。
3. **请求处理**：`async_read_header` → 预检/100-continue → `async_read` → 投递业务线程池
   （Router 解析/鉴权/查库/序列化）→ post 回 strand → `http::async_write` → keep-alive 则回到第 3 步。
4. **清理/停机**：超时/错误/对端关闭 → `do_close`（TLS 发 `close_notify`，带兜底定时器）；
   信号 → `Server::stop`（幂等）→ 排空 → `ioc.stop()` → `run` 返回 → `~Server` 按
   "停传输 → join io 线程 → 回收业务线程池 → 销毁 io_context" 收尾。

## 错误码规范

| 退出码 | 含义 |
|---|---|
| `-1` | 配置/未知非正常退出（含监听地址非法、CIDR 语法错误、未启用任何监听等） |
| `1` | 数据库初始化/连接（`Sql_init` / `Sql_conn`） |
| `2` | 事件循环/传输层初始化（`Reactor_init`） |
| `3` | 监听端口 bind/listen 失败（`Listen_init`） |
| `5` | TLS 配置错误（`Tls_init`：证书/私钥/套件/最低版本） |

> `kErrType` 另含 `Thread_wrong`、`Redis_error`（Redis 运行期降级按 WARN 处理，不触发进程退出）。
> 历史修正：旧枚举里 `Sql_init` 值为 `0`，会让"数据库初始化失败"以退出码 `0` 结束（看起来像成功），已按上表重新编号。

## 文档索引

- **本轮改造汇总与待办**：`CHANGES_AND_TODO.md`（改动清单 + 仍未完成的 TODO，含优先级）
- 接口约定：`API_CONVENTIONS.md`（REST 风格 + 注册/登录/文章/评论约定）
- 协程与 HTTPS 演进规划：`COROUTINE_AND_HTTPS_ROADMAP.md`（施工图与实施状态表）
- 架构不足与改进清单：`TinyServer/ARCHITECTURE_ISSUES.md`
- Redis 键设计与一致性：`REDIS_INTEGRATION.md`
- 旧网络层对照：`TinyServer/legacy/README.md`
- HTTPS 验收脚本：`scripts/verify_https.sh`
- 压测脚手架（生成可对比基线）：`scripts/bench.sh`

## 说明

- 数据库 DDL（`webdatabase` 的 article/comment/user/user_likes 表结构）以项目实际使用的建表语句为准
  （utf8mb4、`user.username` 唯一、`user_likes` 唯一键 `(user_id, article_id)` 等），见 `INTERVIEW_PREP.md` 或本地建表脚本。
