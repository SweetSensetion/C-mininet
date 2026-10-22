---

## 9. 内存与对象生命周期

### 9.1 所有权关系

```
TcpServer ──owns──> Acceptor ──owns──> Socket(listen fd) + Channel
    │
    ├──owns──> EventLoopThreadPool ──owns──> EventLoopThread ──owns(栈)──> EventLoop
    │
    └──shared──> map<string, TcpConnectionPtr>   ← 唯一强引用持有者
                          │
                          ├──owns──> Socket(conn fd)  [unique_ptr]
                          ├──owns──> Channel          [unique_ptr]
                          ├──owns──> Buffer input/output [值成员]
                          └──weak────> Channel::tie_  （反向弱引用，不形成环）
```

| 对象 | 所有权 | 理由 |
| --- | --- | --- |
| `Socket` | `unique_ptr` | fd 独占，无共享语义 |
| `Channel` | `unique_ptr`（由 Connection 持有） | 与连接同生共死 |
| `TcpConnection` | `shared_ptr`（TcpServer 的连接表） | 回调、跨线程任务都可能临时持有 |
| `EventLoop` | 栈对象（EventLoopThread 内） | 生命周期严格等于线程 |
| `Timer` | `shared_ptr`（TimerQueue） | 定时器任务可能跨线程投递 |
| 业务上下文 | `shared_ptr<void>`（Connection 内） | 跟随连接析构 |

### 9.2 为什么必须是 shared_ptr，而不是裸指针

裸指针方案在这个架构下有三个绕不过去的问题：

1. **回调可能比对象活得久**。一个连接关闭后，之前投递到线程池的任务、或已经排进 pending 队列的 `runInLoop` 任务仍持有 `TcpConnection*`。裸指针无法感知对象已析构。
2. **跨线程销毁无法同步**。IO 线程决定关闭连接，但最后一个引用可能握在业务线程手里。只有引用计数能表达"最后一个人负责释放"。
3. **异常路径难覆盖**。业务回调抛异常时，裸指针方案容易漏掉 delete；`shared_ptr` 由栈展开保证释放。

代价也要说清楚：`shared_ptr` 的拷贝是一次原子自增（约 10~20 ns，且会造成缓存行争用），所以在**热路径上传参一律用 `const TcpConnectionPtr&`**，不要按值传递。构造用 `std::make_shared`（一次分配，控制块与对象连续），只有需要 `weak_ptr` 时才用 `new` + `shared_ptr` 构造。

### 9.3 循环引用：Reactor 里最隐蔽的泄漏

典型错误写法：

```cpp
// ❌ 连接自己持有自己的强引用，引用计数永远 >= 1
conn->setMessageCallback([conn](const TcpConnectionPtr&, Buffer*, Timestamp) {
    conn->send(...);          // 捕获了 conn 的 shared_ptr
});
```

`TcpConnection` 持有 `messageCallback_`，回调又持有 `TcpConnection` 的 `shared_ptr`，形成自环，连接关闭后内存与 fd 都不会释放。正确做法有三选一：

1. 回调捕获 `weak_ptr`，用时 `lock()`；
2. 回调捕获裸 `this`（因为回调本来就存在对象内部，对象活着回调才可能被调用）；
3. 用 `Channel::tie()` 的机制：只让 Channel 持有 `weak_ptr`，回调内部用 `shared_from_this()` 临时提升。

本项目库内部统一采用第 2、3 种；示例代码中业务若需跨回调持有连接，应使用 `weak_ptr`。

### 9.4 跨线程析构：为什么不能直接 delete

设想 `handleClose()` 的执行栈：

```
EventLoop::loop -> Channel::handleEvent -> TcpConnection::handleClose -> closeCallback_ -> TcpServer::removeConnection
```

如果 `removeConnection` 在这里直接 `delete conn`，那么栈展开回到 `handleClose` 时，函数还要访问 `state_`、`closeCallback_` 等成员——对象已经没了，典型的 use-after-free。

本项目的处理是两级延迟：

```cpp
void TcpServer::removeConnectionInLoop(const TcpConnectionPtr& conn) {
    { std::lock_guard<std::mutex> lock(mutex_); connections_.erase(conn->name()); }
    // 把销毁推迟到下一轮事件循环，此时当前调用栈已完全退出
    conn->getLoop()->queueInLoop([conn]() { conn->connectDestroyed(); });
}
```

lambda 捕获 `conn` 的 `shared_ptr`，保证对象活到任务真正执行。`queueInLoop` 而不是 `runInLoop` 是关键：同线程时 `runInLoop` 会立即执行，等于没延迟。

### 9.5 为什么不用 GC，也不用内存池

| 方案 | 不采用的理由 |
| --- | --- |
| 追踪式 GC | C++ 没有；即便有，GC 停顿对延迟敏感服务不可接受；而且 **fd 必须确定性关闭**，等 GC 回收等于把 fd 泄漏给整个进程 |
| 引用计数（本项目） | 确定性析构，fd 随对象立即关闭；代价是原子操作与环检测靠人工 |
| 对象池 / 内存池 | 每连接对象数量固定、分配频率低（只在建连/断连时），池化收益 < 5%，但引入容量上限、回收时机、线程安全等复杂度。**优化要打在有热点的地方**，这里没有热点 |
| Arena（按连接分配） | 思路正确（把一条连接的所有小对象放一起），但会与 `shared_ptr` 的所有权模型冲突，暂不引入 |

### 9.6 一个具体的泄漏排查方法

```bash
# 1. 看 fd 是否随连接数回落
ls /proc/$PID/fd | wc -l

# 2. 看堆是否持续增长
heaptrack ./echo_server 9000 4      # 或 valgrind --leak-check=full

# 3. 看是否有 shared_ptr 环：在 TcpConnection 析构里打日志，压测后计数应等于建连数
```

本项目的 `TcpConnection` 析构函数带日志，压测脚本会对比"建连数"与"析构数"，两者不等即为泄漏。

---

## 10. 并发正确性

### 10.1 线程安全边界

| 接口 | 线程安全性 | 说明 |
| --- | --- | --- |
| `EventLoop::runInLoop/queueInLoop` | 线程安全 | 内部加锁 + eventfd 唤醒 |
| `EventLoop::loop/quit` | `quit` 线程安全，`loop` 仅限本线程 | |
| `TcpConnection::send/shutdown/forceClose` | 线程安全 | 内部投递到 IO 线程 |
| `TcpConnection` 其他方法 | 仅限 IO 线程 | 由 `assertInLoopThread` 在 debug 版捕获 |
| `TcpServer::start` | 线程安全（`fetch_add` 保证只执行一次） | |
| `TcpServer::setXxxCallback` | 仅在 `start()` 前调用 | 避免运行期修改回调的竞态 |
| `ThreadPool::run` | 线程安全 | 但不要在池内任务里调用（会死锁） |

### 10.2 锁的粒度

全库只有三把锁，且都不在数据热路径上：

| 锁 | 保护对象 | 持锁时间 |
| --- | --- | --- |
| `EventLoop::mutex_` | `pendingFunctors_` | 一次 `push_back` / 一次 `swap`，O(1) |
| `TcpServer::mutex_` | `connections_` 表 | 一次 map 插入/删除，O(log n) |
| `ThreadPool::mutex_` | `tasks_` 队列 | 一次 push/pop，O(1) |

连接上的读写、解码、发送**完全无锁**——因为线程封闭。这是主从 Reactor 相对"单 Reactor 多线程"最大的结构性优势：后者所有连接共享一个 IO 线程，连接状态要么加锁，要么靠任务队列串行化。

### 10.3 惊群

| 场景 | 现象 | 本项目处理 |
| --- | --- | --- |
| 多线程 accept 同一 listen fd | 新连接到来唤醒所有线程，只有一个成功 | 单 Acceptor，只有 mainReactor 一个线程 accept |
| 多进程共享 listen fd | 同上，且跨进程 | 若要多进程，用 `SO_REUSEPORT` 让内核按四元组哈希分流 |
| 多线程 epoll_wait 同一 epfd | 4.5 之前全部唤醒 | 不共享 epfd；每个 subReactor 独立 epoll 实例 |

### 10.4 原子变量与内存序

只有两处需要原子：

```cpp
std::atomic<bool> looping_, quit_;     // 跨线程读写退出标志
std::atomic<StateE> state_;            // connected() 可能被业务线程调用
std::atomic<int> started_;             // TcpServer::start 幂等
```

其余场景不需要 `memory_order_relaxed` 之类的细调：默认的 `seq_cst` 在 x86 上编译成普通 load/store + 编译器屏障，开销与普通变量几乎相同。**在没有实测证明原子操作是瓶颈之前，不要为了"看起来专业"去改内存序**——这是引入最难排查的 bug 的最快方式。

### 10.5 伪共享

`TcpConnection` 的 `state_` 与 `reading_` 相邻，如果多个线程频繁读 `state_`，可能与写 `reading_` 的线程争同一缓存行。本项目未做 padding，因为连接对象本身的访问几乎都集中在所属 IO 线程，跨线程读 `state_` 的频率很低。若将来 `connected()` 变成高频调用，再把 `state_` 用 `alignas(64)` 单独放一行。

---

## 11. 性能：度量、拆解与优化

### 11.1 怎么测

| 指标 | 工具 | 注意点 |
| --- | --- | --- |
| QPS | 自研 `bench_client`（流水线） | 必须多连接 + 流水线，否则测的是 RTT 不是吞吐 |
| 延迟分布 | 客户端记录每批 RTT，算 P50/P99/P999 | 只看平均值会掩盖长尾 |
| CPU | `perf stat`、`pidstat -t` | 关注 `context-switches` 与 `cpu-migrations` |
| 内存 | `/proc/PID/status` 的 VmRSS、`heaptrack` | 压测前后对比，排除缓慢泄漏 |
| 系统调用 | `strace -c -f` | 看 `epoll_wait`/`read`/`write` 次数是否符合预期 |
| 热点 | `perf record -g` + 火焰图 | 确认时间花在业务还是框架 |

压测纪律：客户端与服务端**不要跑在同一台机器的同一批核上**，否则测到的是调度抖动；回环测试只能看相对变化，不能当作线上容量。

### 11.2 一次请求的关键路径

```
epoll_wait 返回（~2-5 µs 唤醒延迟）
   └─ Channel::handleEvent（函数指针 + 一次 weak_ptr::lock）
        └─ readv（1 次系统调用，~1-2 µs）
             └─ Codec 解析（纯用户态，~100 ns）
                  └─ 业务回调
                       └─ write（1 次系统调用）
                            └─ epoll_ctl(MOD)（仅在关注集合变化时发生）
```

一次 ping-pong 在回环上的端到端延迟约 20~40 µs，其中系统调用与调度占大头，协议解析占比不到 1%。这解释了为什么"把协议解析优化到极致"对吞吐几乎没有帮助——**瓶颈永远在系统调用、内存拷贝和上下文切换**。

### 11.3 优化清单与预期收益

| 优化 | 做法 | 预期收益 | 代价 |
| --- | --- | --- | --- |
| ET + 读干净 | 一次事件循环读，减少 `epoll_wait` 返回次数 | 高并发下 10%~20% | 编码复杂度上升 |
| 写缓冲 + 按需关注 EPOLLOUT | 避免忙等 | 防止 CPU 100% | 多一次内存拷贝 |
| `readv` 两段读 | 减少 buffer 扩容 | 5% 左右 | 栈上 64KB |
| 关闭 Nagle | `TCP_NODELAY` | 消除 40 ms 级尾延迟 | 小包变多，带宽略升 |
| 对象复用（Buffer 不释放） | 读空复位索引 | 减少 malloc 调用 | 常驻内存略高 |
| 批量 accept | 循环到 EAGAIN | 建连风暴下明显 | 单次事件处理变长 |
| CPU 亲和性 | `pthread_setaffinity_np` 绑定 IO 线程 | 减少迁移 5%~10% | 需感知 NUMA 拓扑 |
| 避免 `shared_ptr` 按值传参 | 统一 `const&` | 减少原子操作 | 无 |

### 11.4 实测数据

测试环境：Windows 11 + WSL2，Ubuntu 26.04.1（内核 6.18.54.1-microsoft-standard-WSL2），16 vCPU，GCC 15.2.0，CMake 4.2.3，`ulimit -n 65535`，客户端与服务端同在回环上。

| 场景 | 配置 | 实测结果 | 观察 |
| --- | --- | --- | --- |
| 吞吐 | 4 IO 线程；客户端 4 线程 × 8 连接，流水线深度 32；64B body；运行 5s | **63~66 万 QPS**（两次实测：328.5 万 / 313.7 万次响应），连接失败 0 | CPU 处理效率，非线上容量 |
| 万级连接 | 4 IO 线程；10,000 条空闲长连接 | 服务端 fd **10,016**，VmRSS **35 MB**，线程数 **5**；客户端关闭后进程存活 | 约 3.5 KB/连接（用户态对象 + 缓冲区） |
| 协议正确性 | `codec_test`（逐字节拆包、粘包、零长 body、坏 magic、超长 body） | 5/5 通过 | 无网络依赖，可进 CI |
| 构建 | `cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j16` | 首次编译 0 error，`-Wall -Wextra` 无告警 | GCC 15.2 + C++11 |

复现命令：

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/echo_server 19000 4 &
./build/bench_client 127.0.0.1 19000 4 8 5 64
```

> 这是 WSL2 虚拟机上的回环数据，客户端与服务端共享同一批 CPU，**只能说明实现本身没有明显瓶颈，不能当作线上容量承诺**。真实容量取决于网卡、RPS/RFS、连接分布与业务耗时；正式材料应换到裸金属或独立压测机上重跑，并保留 `uname -a`、`nproc`、`perf stat` 输出。

### 11.5 QPS 上不去时怎么排查

1. **先看是不是客户端瓶颈**：客户端 CPU 打满、连接数太少、没有流水线，都会让服务端"看起来很闲"。用 `top -H` 看客户端线程。
2. **看是不是 IO 线程瓶颈**：`pidstat -t` 观察各线程 CPU，若只有 1 个核忙，说明 `setThreadNum(0)` 或连接分布不均。
3. **看系统调用次数**：`strace -c` 若 `epoll_wait` 次数异常高而 `read` 很少，说明有 fd 一直在触发事件（典型是错误地常关注 EPOLLOUT）。
4. **看锁竞争**：`perf record -e futex` 或 `perf lock`，若 `EventLoop::mutex_` 频繁争用，说明跨线程投递过多，应合并发送。
5. **看内存拷贝**：大 body 场景下 `memcpy` 占比高，考虑 `sendfile`/零拷贝或缩小协议头。

---

## 12. 与同类方案对比

| 方案 | 模型 | 语言/依赖 | 优势 | 相对本项目的差异 |
| --- | --- | --- | --- | --- |
| **本项目** | 主从 Reactor，ET | C++11，仅 pthread | 代码可读、可裁剪、机制完整 | 无 TLS/HTTP，无生产级运维能力 |
| muduo | 主从 Reactor，LT | C++11，Boost 部分组件 | 工业级成熟、文档完善、有 Protobuf/HTTP 扩展 | 本项目可视为其简化教学版；LT vs ET、有无 Codec 生态是主要差异 |
| libevent | Reactor，C | C | 跨平台、事件抽象完整、久经考验 | 回调风格偏 C，缓冲区与连接抽象较弱 |
| Boost.Asio | Proactor 接口 / Reactor 实现 | C++，Boost | 接口优雅、跨平台、支持协程 | 模板错误信息长，抽象层次高，不利于理解底层 |
| nginx | 多进程 + 每进程单 Reactor | C | 静态资源性能极致、模块化 | 面向 HTTP，不做通用长连接业务 |
| Redis | 单 Reactor 单线程 | C | 无锁、内存操作极快 | 单线程模型不适合 CPU 密集业务；靠 IO 多线程（6.0+）补足 |
| Netty | 主从 Reactor | Java/JVM | 生态完善、编解码器丰富 | JVM GC 与内存模型带来另一类问题 |

**为什么 Redis 单线程也能打**：它的操作几乎全是内存读写，单核即可跑满网卡；一旦引入多线程反而要加锁。这说明"线程数"不是性能的决定因素，**瓶颈在哪里**才是。本项目的场景是通用长连接，业务可能耗时，所以必须把 IO 与计算分离。

---

## 13. 适用与不适用场景

| 场景 | 是否适合 | 原因 |
| --- | --- | --- |
| IM / 推送长连接网关 | ✅ 很适合 | 万级空闲连接、低 CPU、二进制协议、需要心跳 |
| 游戏服务器（房间/战斗） | ✅ 适合 | 低延迟、小消息、自定义协议；需另加帧同步逻辑 |
| 自定义 RPC 的传输层 | ✅ 适合 | 长度前缀天然承载 Protobuf/FlatBuffers |
| 设备接入 / IoT | ✅ 适合 | 连接多、单连接流量小；需补 TLS 与鉴权 |
| 静态文件 / 大文件下载 | ❌ 不适合 | 缺 `sendfile` 零拷贝与缓存，应用 nginx |
| 对外 HTTP API | ❌ 不适合 | 无 HTTP 解析、无 keep-alive 语义、无 TLS |
| 短连接高 QPS（如健康检查） | ⚠️ 勉强 | accept 与 TIME_WAIT 成本高，应做连接复用 |
| 计算密集型任务 | ❌ 不适合 | 应把计算放进线程池或独立服务，网络库只负责传输 |
| 需要强隔离的多租户 | ❌ 不适合 | 单进程崩溃影响全部连接，应多进程/多机 |

选型的本质是**匹配瓶颈形态**：连接多而流量小 → IO 多路复用；流量大而连接少 → 多线程 + 零拷贝；计算重 → 分离计算层。没有一个模型在所有维度占优。

---

## 14. 已知局限与演进路线

| 局限 | 影响 | 演进方向 |
| --- | --- | --- |
| 无 TLS | 不能直接暴露公网 | 集成 OpenSSL，在 Buffer 与 Socket 之间加 SSLChannel；或前置 TLS 终结代理 |
| 无 HTTP 支持 | 不能做 Web 服务 | 加 HttpCodec（状态机解析请求行/头/body） |
| 无零拷贝 | 大 body 场景多一次拷贝 | `sendfile`/`splice`；协议头与 body 分离发送 |
| 无多进程 | 单机故障域大、难滚动升级 | `SO_REUSEPORT` + 多 worker + 共享内存统计 |
| ET 下无读预算 | 单个高速连接可能饿死其他连接 | EPOLLONESHOT + 处理完主动重新注册，或按字节预算让出 |
| 定时器 O(n) 取消 | 海量定时器时性能下降 | 改时间轮或维护 `Timer* -> iterator` 索引 |
| 无 io_uring | 系统调用次数仍偏多 | 封装 `Poller` 的 io_uring 后端（接口已抽象好） |
| 日志同步写 | 高频日志会阻塞 | 异步日志：双缓冲 + 后台线程 |

演进顺序建议：**TLS → 异步日志 → 读预算 → io_uring**。前两个是工程必需，后两个是性能深水区。

---

## 15. 核心源码导读

### 15.1 文件清单

| 文件 | 行数量级 | 核心内容 |
| --- | --- | --- |
| `include/net/EventLoop.h` + `src/EventLoop.cc` | 260 | 事件循环、跨线程投递、eventfd 唤醒、定时器接口 |
| `include/net/Channel.h` + `src/Channel.cc` | 190 | 事件到回调的映射、`tie()` 生命周期保护 |
| `include/net/EPollPoller.h` + `src/EPollPoller.cc` | 170 | epoll 三件套封装、fd 状态机（ADD/MOD/DEL） |
| `include/net/Buffer.h` + `src/Buffer.cc` | 190 | 读写索引、`readv`、扩容与复位 |
| `include/net/ThreadPool.h` + `src/ThreadPool.cc` | 170 | 有界队列、条件变量、优雅退出 |
| `include/net/TcpConnection.h` + `src/TcpConnection.cc` | 420 | 状态机、ET 读循环、部分写、背压 |
| `include/net/TcpServer.h` + `src/TcpServer.cc` | 190 | 连接分配、连接表、延迟销毁 |
| `include/net/Protocol.h` + `src/Codec.cc` | 200 | 16 字节头、编解码状态机 |
| `include/net/Acceptor.h` + `src/Acceptor.cc` | 140 | 循环 accept、EMFILE 处理 |
| `include/net/Socket.h` + `src/Socket.cc` | 180 | fd RAII、socket 选项 |

### 15.2 建议阅读顺序

1. `EventLoop::loop()` —— 先看主循环的四步，建立整体骨架；
2. `EPollPoller::poll()` —— 理解事件从内核到 `activeChannels_` 的路径；
3. `Channel::handleEvent()` —— 理解事件如何变成回调；
4. `Acceptor::handleRead()` → `TcpServer::newConnection()` —— 理解连接如何被创建和分配；
5. `TcpConnection::handleRead/handleWrite` —— 理解数据如何流动；
6. `Codec::onMessage()` —— 理解字节流如何变成消息；
7. `TcpServer::removeConnection()` —— 理解对象如何安全销毁。

### 15.3 文档与代码的对应关系

| 文档章节 | 对应实现 |
| --- | --- |
| 5.2 事件循环主体 | `EventLoop::loop` |
| 5.3 跨线程唤醒 | `EventLoop::wakeup` / `handleWakeup` |
| 5.4 runInLoop | `EventLoop::queueInLoop` |
| 5.5 定时器 | `TimerQueue::nextTimeoutMs` / `handleExpired` |
| 5.6 事件处理期间销毁 | `Channel::handleEvent` + `TcpServer::removeConnectionInLoop` |
| 6.2 线程池 | `ThreadPool::workerLoop` |
| 7.2 读与 EAGAIN | `TcpConnection::handleRead` |
| 7.3 部分写 | `TcpConnection::sendInLoop` / `handleWrite` |
| 8.3 协议头 | `Protocol::encode` |
| 8.4 解码状态机 | `Codec::onMessage` |
| 8.5 Buffer | `Buffer::readFd` / `makeSpace` |

### 15.4 构建与验证

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure     # 协议单测

./build/echo_server 9000 4                     # 4 个 IO 线程
./build/bench_client 127.0.0.1 9000 4 8 10 64  # 4 线程 × 8 连接 × 10 秒
```

`codec_test` 覆盖了逐字节喂入（极端拆包）、一次喂入多帧（极端粘包）、零长 body、非法 magic、超长 body 五种情况。**没有单测的网络库只能叫 demo**，这个测试是面试时证明"我验证过边界"的最直接材料。

---

## 16. 附录

### A. 核心类关系

| 类 | 职责 | 关键成员 | 与其他类的关系 |
| --- | --- | --- | --- |
| `TcpServer` | 服务器门面，装配 acceptor 与线程池 | `loop_`、`acceptor_`、`threadPool_`、`connections_` | 拥有 Acceptor 与 EventLoopThreadPool；连接表持有 `TcpConnectionPtr` |
| `Acceptor` | 只负责 accept | `acceptSocket_`、`acceptChannel_`、`idleFd_` | 在 baseLoop 上注册 listen fd；新连接回调 TcpServer |
| `TcpConnection` | 一条已建立的连接 | `socket_`、`channel_`、`inputBuffer_`、`outputBuffer_`、`state_` | 绑定唯一 EventLoop；持有 Socket / Channel / 两个 Buffer |
| `EventLoop` | 事件循环与任务调度 | `poller_`、`timerQueue_`、`wakeupChannel_`、`pendingFunctors_` | 拥有 Poller 与 TimerQueue；管理其上的 Channel |
| `Channel` | fd 与回调的绑定 | `fd_`、`events_`、`index_`、`tie_` | 属于某个 EventLoop；`tie()` 以 weak_ptr 指向所有者 |
| `Poller` / `EPollPoller` | IO 复用抽象与 epoll 实现 | `epollfd_`、`events_`、`channels_` | 被 EventLoop 拥有；EPollPoller 继承 Poller |
| `TimerQueue` | 定时器有序集合 | `timers_`、`cancelingTimers_` | 被 EventLoop 拥有；到期时间决定 `epoll_wait` 超时 |
| `Buffer` | 用户态读写缓冲 | `buffer_`、`readerIndex_`、`writerIndex_` | 被 TcpConnection 持有 |
| `ThreadPool` | 业务任务线程池 | `tasks_`、`mutex_`、`notEmpty_`、`notFull_` | 独立于 IO 线程，由业务层投递任务 |

> 这张表就是源码的"地图"：按"谁拥有谁、谁调用谁"读一遍，再看第 15.2 节的阅读顺序，基本可以在半天内掌握全部实现。

### B. 对外接口清单

```cpp
// 服务器
TcpServer(EventLoop*, const InetAddress&, const std::string& name, Option);
void setThreadNum(int);
void setConnectionCallback(ConnectionCallback);
void setMessageCallback(MessageCallback);
void setWriteCompleteCallback(WriteCompleteCallback);
void start();

// 连接
void send(const void*, size_t);
void send(const std::string&);
void shutdown();
void forceClose();
void setTcpNoDelay(bool);
Buffer* inputBuffer();
void setContext(const std::shared_ptr<void>&);

// 事件循环
void loop();
void quit();
void runInLoop(Functor);
void queueInLoop(Functor);
TimerId runAfter(double, TimerCallback);
TimerId runEvery(double, TimerCallback);
void cancel(TimerId);

// 协议
bool Codec::onMessage(Buffer*, std::vector<protocol::Message>*);
void protocol::encode(const protocol::Message&, Buffer*);
```

### C. 术语表

| 术语 | 含义 |
| --- | --- |
| Reactor | 就绪通知模型：内核告诉应用"可以读了"，由应用自己读 |
| Proactor | 完成通知模型：内核替应用读完，再通知"读完了" |
| LT / ET | 水平触发 / 边沿触发，决定同一份数据是否重复通知 |
| 粘包 / 拆包 | 多条消息被合并成一次读 / 一条消息被拆成多次读 |
| one loop per thread | 每个线程一个事件循环，连接绑定到唯一循环 |
| 线程封闭 | 对象只被一个线程访问，从根上消除数据竞争 |
| 背压 | 下游处理不过来时，向上游施加压力（丢包/拒绝/限流） |
| 惊群 | 多个等待者被同一事件全部唤醒，只有一个能处理 |
| 半包 | 一条完整消息只到达了一部分 |

### D. 面试问答（高频追问）

**Q1：epoll 为什么比 select 快？**
不是"快"，是**复杂度不同**。select 每次调用要把 fd_set 全量拷进内核、内核线性扫描所有 fd、返回后再由用户态扫描一遍；epoll 把"注册"和"等待"分离，fd 只在 `epoll_ctl` 时进红黑树，事件到达时通过回调挂到就绪链表，`epoll_wait` 只拷贝就绪项，复杂度 O(k) 而不是 O(n)。另外 select 有 `FD_SETSIZE=1024` 的编译期上限。

**Q2：为什么用 ET？踩过什么坑？**
ET 减少重复通知，空闲连接多时 CPU 更省。坑有三个：必须循环读到 EAGAIN；accept 也必须循环；写方向不能常关注 EPOLLOUT，否则 `epoll_wait` 立即返回形成忙等。这三条本质是同一条：**ET 只报状态变化，不报状态本身**。

**Q3：新连接为什么要投递到 subReactor，不能直接在那里构造 TcpConnection 吗？**
不能。`TcpConnection` 的构造会创建 `Channel` 并注册到 epoll，而 `epoll_ctl` 必须作用在正确的 epoll 实例上；同时 `Channel` 的回调、缓冲区都约定只被所属线程访问。如果在 mainReactor 线程构造，对象就同时被两个线程触碰，线程封闭的前提被破坏。

**Q4：一条连接的数据会被多个线程同时处理吗？**
不会。连接在建立时被分配给某个 subReactor，之后所有读写、解码、定时器都在该线程串行执行。只有显式投递到线程池的业务计算会离开该线程，且结果要再投递回来发送。

**Q5：shared_ptr 循环引用在这个项目里怎么产生的？怎么解决？**
如果连接的回调捕获了 `TcpConnectionPtr`，而回调又存放在 `TcpConnection` 成员里，就形成自环。解决办法是回调捕获裸 `this` 或 `weak_ptr`；库内部用 `Channel::tie()` 持有 `weak_ptr`，需要时 `lock()` 提升，既不形成环又能保活。

**Q6：回调里 delete 连接为什么会崩？tie 的原理是什么？**
因为 `handleEvent` 的栈帧还在使用 `this`。`tie` 让 Channel 持有一个指向所有者的 `weak_ptr`，`handleEvent` 开始时 `lock()` 出 `shared_ptr` 放在栈上，回调里即使把外部所有强引用都释放，对象也要等这个 guard 析构才销毁。

**Q7：线程池为什么必须有界？**
无界队列在过载时表现为内存持续增长直到 OOM，而且崩溃发生在最糟糕的时刻。有界队列把过载转换成可观测的拒绝（`run` 返回 false），让调用方决定降级策略：返回忙、断开连接或丢弃低优先级任务。

**Q8：为什么不用无锁队列？**
任务粒度通常在微秒级以上，未竞争的 mutex 约 20 ns，占比可忽略。无锁 MPMC 需要安全的内存回收（hazard pointer/epoch），C++11 缺少 `atomic<shared_ptr>`，自己实现容易引入 ABA 和内存序错误。**在锁不是瓶颈的地方引入无锁，是用正确性换性能**。

**Q9：粘包怎么解决？你的协议头为什么是 16 字节？**
TCP 是字节流，必须在应用层定义边界。三种方案：定长（浪费）、分隔符（要转义、要扫描）、长度前缀（O(1)、二进制安全）。16 字节 = 4(magic) + 1(ver) + 1(type) + 2(flags) + 4(seq) + 4(bodyLen)，正好 4 字节对齐、无填充，且能覆盖版本演进、请求匹配、扩展标志。

**Q10：bodyLen 不设上限会怎样？**
攻击者发一个 16 字节的包声明 `bodyLen=0xFFFFFFFF`，服务端就会尝试分配 4GB，一次请求打爆内存。上限 8MB 是安全边界，超限直接断连。同理，半包也要限制单连接缓冲上限，否则"半包洪水"同样能打爆内存。

**Q11：发送缓冲区什么时候关注 EPOLLOUT？一直关注会怎样？**
只在输出缓冲非空时关注，写完立刻取消。一直关注会让 `epoll_wait` 因为"可写"这个常态条件立刻返回，事件循环变成忙等，CPU 直接 100% 而吞吐为零。

**Q12：1 万连接大概占多少内存？**
用户态：每连接约 1~2 KB（TcpConnection 对象 + Channel + 两个 Buffer 初始 1KB）；内核态：每个 socket 的读写缓冲区，默认几十 KB 且可自动调优。合计 1 万连接大约 100~500 MB，其中大头是内核缓冲，可以通过 `SO_RCVBUF` 或 `tcp_rmem` 调整。

**Q13：QPS 上不去怎么排查？**
按"客户端 → IO 线程 → 系统调用 → 锁 → 拷贝"的顺序：先确认客户端不是瓶颈（CPU、连接数、流水线）；再看是否只有一个 IO 线程忙（`pidstat -t`）；再看 `epoll_wait` 次数是否异常（错误关注 EPOLLOUT 的典型症状）；再看 futex 争用；最后看大 body 的 `memcpy` 占比。

**Q14：你这个库和 muduo 差在哪？**
muduo 用 LT、有更完整的定时器与日志、Protobuf/HTTP 扩展、经过生产验证；本项目的差异是 ET + 显式读循环、更薄的抽象（无 Boost）、以及把"为什么"写进了注释和设计文档。功能上确实少 TLS/HTTP/多进程，这些在演进路线里写明了优先级。

---

## 结语

这个项目的价值不在于"又一个网络库"，而在于把 Linux 网络编程里真正决定正确性与性能的机制完整走了一遍：**事件如何从内核到达回调、连接如何被安全地创建与销毁、字节流如何变成消息、线程之间如何不共享状态**。

面试时最容易被追问的从来不是"你用了什么"，而是"如果换个条件，你还会这么选吗"。这份文档里每一处"被淘汰方案在什么场景更优"的说明，就是为这个问题准备的。
