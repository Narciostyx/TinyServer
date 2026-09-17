# legacy/ —— 已被 Asio 传输层取代的旧网络层（保留仅供对照，不参与编译）

这些文件是"**主/从 Reactor + epoll + 线程池**"时代的实现，2026 年迁移到
**Boost.Asio（io_context + Asio SSL）**后已从构建中移除（`CMakeLists.txt` 与
`TinyServer.vcxproj` 都不再引用），保留在此是为了：

1. 对照阅读：理解为什么需要迁移（见 `../COROUTINE_AND_HTTPS_ROADMAP.md` 与 `../README.md`）。
2. 面试/文档引用：里面实现过的 ET 半包、连接级串行化、空闲超时最小堆、EAGAIN→EPOLLOUT 续发等机制。

> **已全部标注 `[[deprecated]]`**：`SubReactor`、`MainReactor`、`Connection`、`Acceptor` 四个类
> 都带上了废弃属性（与 `../thread.hpp`、`../mutex.hpp`、`../alloc.hpp` 的处理方式一致），
> 并在属性消息里指明替代实现。若将来把本目录加回构建，任何使用都会产生编译期告警。

| 文件 | 原职责 | 现在的替代者 |
|---|---|---|
| `reactor.hpp/.cpp` | `MainReactor`（accept + 轮询分发）/ `SubReactor`（每线程一台 epoll、跨线程 `post`、最小堆空闲超时、`flush_write`） | `../asio_transport.hpp/.cpp`（`io_context` + `tcp::acceptor` + strand + `signal_set`） |
| `connection.hpp` | 连接会话容器：互斥锁保护的读写缓冲、`task_in_flight_`/`data_while_busy_` 原子、`owner_` 指针 | `../http_session.hpp`（`HttpSession<Stream>`：socket/buffer/parser/timer 全为成员，回调在 strand 上串行，无需加锁） |
| `acceptor.hpp/.cpp` | 裸 `accept` 封装、非阻塞 fd 设置、硬编码 127.0.0.1 白名单 | `asio_transport.cpp` 的 `open_listener`/`do_accept` + `Listen_address`/`Allow_peers` 配置（CIDR 白名单） |

## 迁移时被修掉的已知问题

* **fd 编号复用导致的跨连接串包**：旧 `SubReactor::flush_write` 按 fd 编号查 `fd_contexts_` 判断
  "连接是否还在"，而 `close_expired` / `EPOLLRDHUP` / `recv()==0` 三条路径都不检查 `task_in_flight_`。
  fd 被内核复用后，旧连接的响应可能被发给新连接的客户端。Asio 版以"会话对象即身份"彻底消除该问题。
* **`EPOLLRDHUP` 抢先于 `EPOLLIN` 处理会丢请求**：同一批事件同时带 `EPOLLIN|EPOLLRDHUP` 时
  旧代码直接关闭而不 `recv`，客户端发完请求即 FIN 就拿不到响应。Asio 的 `async_read` 语义天然正确。
* **同包内第二个 keep-alive 请求被丢弃**：旧 `parse_http_request` 用 `eager(true)` + 一次性解析，
  剩余字节被丢弃。Asio 版用 `flat_buffer` + parser 复用，天然支持 pipelining。

确认不再需要后，整个目录可以直接删除（仓库当前不是 git 仓库，删除前请先备份）。
