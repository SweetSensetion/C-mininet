---

## 5. EventLoop：事件循环的实现

### 5.1 核心不变量

整个库的正确性建立在一个不变量上：**一个 `EventLoop` 对象终生绑定一个线程，一个 `TcpConnection` 终生绑定一个 `EventLoop`**。由此推出：

- 连接的读写、解码、超时处理全部在该连接所属线程串行执行，无需加锁；
- 任何跨线程操作（新连接建立、任务投递、连接销毁）都必须通过 `runInLoop` / `queueInLoop` 排队到目标线程，而不是直接调用；
- `EventLoop` 的 `loop()` 只能由创建它的线程调用，其他线程调用属于未定义行为（用 `assertInLoopThread` 在 debug 版捕获）。

这条不变量是用"少量跨线程投递"换掉了"大量细粒度锁"，是后面所有并发讨论的前提。

### 5.2 事件循环主体

```cpp
void EventLoop::loop() {
    assertInLoopThread();
    looping_ = true;
    quit_ = false;

    while (!quit_) {
        activeChannels_.clear();
        // 1) 阻塞等待事件，超时时间由最近的定时器决定
        pollReturnTime_ = poller_->poll(timerQueue_->nextTimeoutMs(), &activeChannels_);

        // 2) 分发就绪事件
        for (Channel* channel : activeChannels_) {
            currentActiveChannel_ = channel;
            channel->handleEvent(pollReturnTime_);
        }
        currentActiveChannel_ = nullptr;

        // 3) 执行跨线程投递的待办任务
        doPendingFunctors();
    }
    looping_ = false;
}
```

三个阶段的顺序有讲究：**先处理 IO，再处理待办队列**。如果反过来，一个"注册了就要立即触发"的任务可能在本轮 poll 之前修改了 Channel 状态，导致本轮事件与对象状态不一致。而放在 IO 之后，本轮所有事件都已经在一致的对象状态下处理完毕。

`doPendingFunctors` 采用"交换后执行"的写法，避免在执行任务时长时间持锁：

```cpp
void EventLoop::doPendingFunctors() {
    std::vector<Functor> functors;
    callingPendingFunctors_ = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        functors.swap(pendingFunctors_);   // 持锁时间 O(1)，只换指针
    }
    for (const Functor& f : functors) f();
    callingPendingFunctors_ = false;
}
```

注意 `callingPendingFunctors_` 的作用：如果任务 A 在执行过程中又调用了 `queueInLoop(B)`，而此时正处于 `doPendingFunctors` 中，B 被放进队列后本轮不会再执行，必须主动 `wakeup()`，否则 B 要等到下一次事件到来才被执行，极端情况下（无后续 IO）会永久挂起。

### 5.3 跨线程唤醒：为什么用 eventfd

主线程 `epoll_wait` 阻塞时，其他线程往队列里塞了任务，必须让 `epoll_wait` 立刻返回。做法是创建一个"唤醒 fd"注册到 epoll，写它即可唤醒。

| 方案 | 优点 | 缺点 | 结论 |
| --- | --- | --- | --- |
| `pipe` | 通用、可移植 | 占 2 个 fd；需要读空管道；读写缓冲区语义较重 | 可用但浪费 |
| `eventfd` | 1 个 fd、8 字节计数、内核态信号量语义、`EFD_NONBLOCK` | Linux 专有 | **采用** |
| `socketpair` | 可移植、可传数据 | 占 2 个 fd | 不必要 |
| `pthread_kill` + 信号 | 无 fd | 信号处理函数只能调用 async-signal-safe 函数；信号会合并丢失；打断系统调用引入 EINTR | 危险，不用 |
| `timerfd` 定时唤醒 | 可做超时 | 有延迟，不实时 | 不适用 |

```cpp
EventLoop::EventLoop() : wakeupFd_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {
    wakeupChannel_.setReadCallback([this](Timestamp) { handleWakeup(); });
    wakeupChannel_.enableReading();
}

void EventLoop::wakeup() {
    uint64_t one = 1;
    ssize_t n = ::write(wakeupFd_, &one, sizeof one);  // 写失败只可能是 EAGAIN，可忽略
    (void)n;
}

void EventLoop::handleWakeup() {
    uint64_t value;
    ssize_t n = ::read(wakeupFd_, &value, sizeof value); // 必须读，否则 level-trigger 下会一直触发
    (void)n;
}
```

这里用 eventfd 而非 pipe 的实际收益：fd 数量减半（1 万连接时每个 loop 省 1 个 fd 不显著，但代码路径更短），且 eventfd 的读写是 8 字节定长，不会出现"管道写满阻塞"的边界情况。

### 5.4 runInLoop 与 queueInLoop

```cpp
void EventLoop::runInLoop(Functor cb) {
    if (isInLoopThread()) cb();                    // 同线程：直接执行，零开销
    else queueInLoop(std::move(cb));               // 跨线程：入队 + 唤醒
}

void EventLoop::queueInLoop(Functor cb) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pendingFunctors_.push_back(std::move(cb));
    }
    if (!isInLoopThread() || callingPendingFunctors_) wakeup();
}
```

这两个函数的区别只有一行，但决定了大量代码的正确性：

- `runInLoop` 用于"我不确定自己在哪个线程"的场景，比如 `TcpConnection::send()` 可能被业务线程调用，也可能被 IO 线程自己调用。
- `queueInLoop` 用于"一定要异步"的场景，典型是延迟销毁对象——如果同步销毁，调用栈还在该对象的成员函数里，`delete this` 之后栈上的 `this` 立刻悬空。

### 5.5 定时器：最小堆 + epoll 超时

定时器有三种常见实现，对比如下：

| 方案 | 插入 | 取最近到期 | 取消 | 精度 | 适用 |
| --- | --- | --- | --- | --- | --- |
| 有序链表 | O(n) | O(1) | O(1) | 高 | 定时器极少 |
| 最小堆 | O(log n) | O(1) | O(n)（需惰性删除） | 高 | 通用 |
| 时间轮 | O(1) | O(1) | O(1) | 取决于槽粒度 | 海量定时器（如 Netty HashedWheelTimer） |
| `timerfd` | O(log n) | 内核 | 需重新 arm | 最高 | 需要与其他 fd 统一事件源 |

本项目用 `std::set<std::pair<Timestamp, Timer*>>`（红黑树，等价于有序最小堆且支持 O(log n) 取消），并把"最近到期时间"换算成 `epoll_wait` 的 timeout：

```cpp
int TimerQueue::nextTimeoutMs() const {
    if (timers_.empty()) return kDefaultWaitMs;          // 无定时器则长睡（可被 wakeup 打断）
    Timestamp now = Timestamp::now();
    Timestamp when = timers_.begin()->first;
    if (when <= now) return 0;                            // 已到期，立即返回处理
    int64_t ms = (when - now) / 1000;                     // 微秒转毫秒
    return static_cast<int>(std::min<int64_t>(ms, kMaxWaitMs));
}
```

这里有一个容易漏掉的细节：**如果在 `epoll_wait` 睡眠期间新增了一个更早到期的定时器，必须 `wakeup()`**，否则事件循环会按旧的超时时间睡过头，定时器延迟触发。因此 `addTimer` 在发现新定时器成为最早到期项时，要主动唤醒 loop。

`timerfd` 方案的优势是把超时变成"fd 可读事件"，可以和其他事件统一处理，且不受 `epoll_wait` 超时精度影响；代价是多一个 fd、多一次 `read`，以及每次修改到期时间都要 `timerfd_settime`。在万级连接、定时器数量不大的场景下，两者的差别在噪声范围内，选择实现更简单的 timeout 方案。

### 5.6 事件处理期间的对象销毁

这是 Reactor 实现中最容易写出 use-after-free 的地方。场景：`handleRead` 中解码出一个"关闭连接"的指令，回调里直接 `delete conn`，但 `handleRead` 的栈帧还在，后续代码继续访问成员变量。

三道防线：

1. **`Channel::tie()`**：Channel 持有一个指向所有者（TcpConnection）的 `weak_ptr`，`handleEvent` 开始时尝试 `lock()` 成 `shared_ptr`，保证整个回调期间对象不被销毁。
2. **延迟销毁**：`TcpServer::removeConnection` 不在回调中直接 delete，而是 `ioLoop->queueInLoop([conn]{ conn->connectDestroyed(); })`，并把 `shared_ptr` 拷贝进 lambda，让对象活到任务执行完毕。
3. **状态机保护**：`TcpConnection` 用 `kConnecting / kConnected / kDisconnecting / kDisconnected` 四态，重复的关闭请求被幂等忽略。

```cpp
void Channel::handleEvent(Timestamp receiveTime) {
    std::shared_ptr<void> guard;
    if (tie_.lock()) guard = tie_.lock();   // 保活，guard 析构时才可能真正释放
    if (revents_ & (EPOLLIN | EPOLLPRI)) {
        if (readCallback_) readCallback_(receiveTime);
    }
    // ... 其他事件
}
```

---

## 6. 线程模型与线程池

### 6.1 线程划分

| 线程 | 数量 | 职责 | 是否阻塞 |
| --- | --- | --- | --- |
| mainReactor | 1 | accept、分发新连接 | 只阻塞在 `epoll_wait` |
| subReactor | N（通常 = CPU 核数） | 本组连接的读写、解码、定时器 | 只阻塞在 `epoll_wait` |
| 业务线程池 | M（通常 = CPU 核数） | 耗时计算、数据库访问、复杂编解码 | 可以阻塞 |
| 日志线程 | 0~1 | 异步落盘 | 阻塞在条件变量 |

关键原则：**IO 线程里绝不做可能阻塞的事**。一次 10 ms 的数据库查询如果放在 IO 线程，会让该线程上所有连接（可能上千个）一起延迟 10 ms。这就是"业务线程池"存在的唯一理由——把不可控的耗时挪出事件循环。

### 6.2 线程池实现

生产者-消费者模型，核心只有三样东西：任务队列、互斥锁、条件变量。

```cpp
class ThreadPool {
public:
    explicit ThreadPool(size_t threadCount, size_t maxQueueSize = 10000)
        : running_(false), maxQueueSize_(maxQueueSize) {
        threads_.reserve(threadCount);
    }

    void start() {
        running_ = true;
        for (size_t i = 0; i < threads_.capacity(); ++i)
            threads_.emplace_back(&ThreadPool::workerLoop, this);
    }

    bool run(Task task) {                       // 返回 false 表示队列已满（背压）
        {
            std::unique_lock<std::mutex> lock(mutex_);
            notFull_.wait(lock, [this] {
                return !running_ || tasks_.size() < maxQueueSize_;
            });
            if (!running_) return false;
            tasks_.push_back(std::move(task));
        }
        notEmpty_.notify_one();
        return true;
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_ = false;
        }
        notEmpty_.notify_all();
        notFull_.notify_all();
        for (std::thread& t : threads_) if (t.joinable()) t.join();
    }

private:
    void workerLoop() {
        for (;;) {
            Task task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                notEmpty_.wait(lock, [this] { return !running_ || !tasks_.empty(); });
                if (!running_ && tasks_.empty()) return;   // 优雅退出：先清空队列再退
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            notFull_.notify_one();
            try { task(); }
            catch (const std::exception& e) { /* 记录日志，绝不让异常逃出线程 */ }
        }
    }

    std::vector<std::thread> threads_;
    std::deque<Task> tasks_;
    std::mutex mutex_;
    std::condition_variable notEmpty_, notFull_;
    bool running_;
    size_t maxQueueSize_;
};
```

几个容易被忽略的正确性细节：

- **`wait` 必须带谓词**：条件变量存在虚假唤醒，裸 `wait` 会在没有任务时返回，导致 `pop_front` 空队列崩溃。
- **任务异常必须捕获**：异常逃出线程函数会直接 `std::terminate` 整个进程，一个业务 bug 就能打挂服务器。
- **`stop` 先置标志再 `notify_all`**：只 `notify_one` 会让其余线程永远等待。
- **退出时先清空队列**：正在排队的任务应该执行完再退，否则会出现"请求已受理但从未处理"的静默丢失。

### 6.3 有界队列与背压

任务队列必须是**有界**的。无界队列在过载时的行为是：内存持续增长 → OOM 被内核 kill，且是在最糟糕的时刻崩溃。有界队列则把过载转化为可观测的拒绝：

```
队列长度上限 10000，达到上限后：
  方案 A：run() 返回 false，调用方降级（返回"服务繁忙"、断开连接、丢弃低优先级任务）
  方案 B：阻塞生产者（不可用于 IO 线程，会把整个 loop 拖住）
  方案 C：丢弃最老任务（适合实时性要求高的场景）
```

本项目默认 A，因为它把"过载"这个事实显式暴露给上层，而不是悄悄积压。

### 6.4 为什么不用无锁队列 / 协程 / std::async / 进程池

| 替代方案 | 为什么不用 | 它在什么场景反而更好 |
| --- | --- | --- |
| 无锁 MPMC 队列 | 需要 hazard pointer / epoch 回收来安全释放节点，C++11 没有 `atomic<shared_ptr>`；ABA 问题与内存序错误极难调试；而任务粒度通常在微秒级以上，一把未竞争的 `mutex` 开销仅约 20 ns | 单任务 < 100 ns 的超高频场景（如金融行情分发），或线程数远超核数时 |
| 协程（ucontext / C++20 coroutine） | C++11 无语言级协程；有栈协程每协程需独立栈（几十 KB 起），1 万连接就是数百 MB；需 hook socket 系统调用，调试栈不直观 | IO 密集且业务逻辑有长调用链（如网关聚合多个后端），代码可读性收益巨大 |
| `std::async` | 执行策略不确定（可能每次新建线程）、返回的 `future` 析构会阻塞等待、无法限制队列长度、异常传播语义隐晦 | 一次性、低频、结果马上要用的并行计算 |
| 进程池 | 跨进程传递任务需序列化 + IPC，延迟高一个数量级；共享连接状态困难 | 需要故障隔离或绕过 GIL（如 Python 多进程） |
| 固定大小线程池（本项目） | 线程数固定，无法应对突发 | 通用后端服务的默认选择 |

### 6.5 线程数怎么定

| 线程类型 | 建议数量 | 依据 |
| --- | --- | --- |
| subReactor | = 物理核数（不含超线程） | IO 线程几乎不阻塞，等于核数可跑满；超过核数只增加切换 |
| 业务线程池 | = 核数（CPU 密集）或 核数 × 2（IO 密集） | IO 密集时线程常阻塞，需要超额订阅 |
| mainReactor | 1 | accept 是极轻的操作，多线程反而引入惊群 |

一个常见的错误是"线程越多吞吐越高"。实测中，4 核机器上把 subReactor 从 4 提到 16，QPS 通常不升反降 10%~20%，因为 `epoll_wait` 的唤醒、缓存行迁移与调度开销吃掉了收益。

---

## 7. 非阻塞 IO 与 Socket 编程

### 7.1 创建监听 socket

```cpp
int Socket::createNonblockingOrDie(sa_family_t family) {
    int sockfd = ::socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (sockfd < 0) { /* LOG_FATAL */ }
    return sockfd;
}

void Acceptor::listen() {
    ::bind(listenFd_, addr_.getSockAddr(), addr_.getSockLen());
    ::listen(listenFd_, SOMAXCONN);
    listenChannel_.enableReading();     // ET 模式下必须循环 accept
}
```

为什么用 `SOCK_NONBLOCK | SOCK_CLOEXEC` 而不是创建后再 `fcntl`：

1. **原子性**：`socket()` 返回的瞬间就是非阻塞的，不存在"刚创建还没设置标志"的窗口。
2. **避免 fd 泄漏**：`CLOEXEC` 防止 `fork + exec` 时把监听 fd 复制给子进程（经典的安全隐患：子进程继承了 listen fd，导致端口无法释放）。
3. **少两次系统调用**，在每秒建连数很高时是可测量的差异。

`accept4()` 同理，直接带 `SOCK_NONBLOCK | SOCK_CLOEXEC` 一次完成。

### 7.2 读：处理 EAGAIN 与半包

```cpp
void TcpConnection::handleRead(Timestamp receiveTime) {
    int savedErrno = 0;
    ssize_t n = inputBuffer_.readFd(channel_->fd(), &savedErrno);
    if (n > 0) {
        messageCallback_(shared_from_this(), &inputBuffer_, receiveTime);
    } else if (n == 0) {
        handleClose();                 // 对端关闭（FIN）
    } else {
        errno = savedErrno;
        if (errno != EAGAIN && errno != EWOULDBLOCK) handleError();
        // EAGAIN 是正常情况：数据读完了
    }
}
```

`read` 返回 0 表示**有序关闭**（收到 FIN），此时不能立刻 `close`，因为可能还有未发送的数据；正确做法是先 `shutdown(SHUT_WR)`，等写缓冲清空后再关闭。

### 7.3 写：部分写与发送缓冲区

`write` 在非阻塞 socket 上**不保证写完**，返回值可能小于请求长度，也可能返回 `EAGAIN`。必须维护应用层发送缓冲区：

```cpp
void TcpConnection::sendInLoop(const void* data, size_t len) {
    ssize_t nwrote = 0;
    size_t remaining = len;
    bool faultError = false;

    // 1) 若发送缓冲区为空，先尝试直接写，避免一次多余的拷贝
    if (!channel_->isWriting() && outputBuffer_.readableBytes() == 0) {
        nwrote = ::write(channel_->fd(), data, len);
        if (nwrote >= 0) {
            remaining = len - nwrote;
            if (remaining == 0 && writeCompleteCallback_)
                writeCompleteCallback_(shared_from_this());
        } else {
            nwrote = 0;
            if (errno != EWOULDBLOCK) {
                if (errno == EPIPE || errno == ECONNRESET) faultError = true;
            }
        }
    }

    // 2) 没写完的部分进入缓冲区，并开始关注 EPOLLOUT
    if (!faultError && remaining > 0) {
        outputBuffer_.append(static_cast<const char*>(data) + nwrote, remaining);
        if (!channel_->isWriting()) channel_->enableWriting();
    }
}
```

**EPOLLOUT 的关注策略是性能关键**：发送缓冲区可写在 99% 的时间里都是成立的，如果一直关注 `EPOLLOUT`，`epoll_wait` 会立刻返回，事件循环变成忙等，CPU 直接打满。所以规则是"**有积压才关注，写完立刻取消**"：

```cpp
void TcpConnection::handleWrite() {
    if (!channel_->isWriting()) return;
    ssize_t n = ::write(channel_->fd(), outputBuffer_.peek(), outputBuffer_.readableBytes());
    if (n > 0) {
        outputBuffer_.retrieve(n);
        if (outputBuffer_.readableBytes() == 0) {
            channel_->disableWriting();              // 关键：取消关注
            if (writeCompleteCallback_) writeCompleteCallback_(shared_from_this());
            if (state_ == kDisconnecting) shutdownInLoop();
        }
    } else if (n < 0 && errno != EAGAIN) {
        handleError();
    }
}
```

### 7.4 关闭与半关闭

TCP 是全双工，关闭有两个方向。正确的关闭序列：

```
应用决定关闭
   │
   ├─ shutdown(fd, SHUT_WR)  → 发送 FIN，进入 FIN_WAIT_1（还能继续读）
   │       │
   │       └─ 对端读到 EOF，处理完剩余请求后也关闭 → 收到对端 FIN
   │
   └─ 读端收到 FIN（read 返回 0）→ 双方都关 → close(fd)
```

直接 `close` 的风险是：接收缓冲区里可能还有对端未处理完的数据，内核会发 RST，导致对端已发出的响应丢失（"连接被重置"错误的常见来源）。因此本项目在 `handleClose` 中先 `shutdown(SHUT_WR)`，等写缓冲清空后再 `close`。

另外必须忽略 `SIGPIPE`：向已关闭的连接写数据会触发该信号，默认行为是**终止进程**。两种做法：`signal(SIGPIPE, SIG_IGN)` 全局忽略，或写时用 `MSG_NOSIGNAL`。本项目两者都做，因为第三方代码可能重置信号处理。

### 7.5 关键 socket 选项

| 选项 | 设置 | 原因 |
| --- | --- | --- |
| `SO_REUSEADDR` | 1 | 服务器重启时跳过 `TIME_WAIT`，否则 `bind` 报 `EADDRINUSE` 要等 60 秒 |
| `SO_REUSEPORT` | 视部署而定 | 多进程/多线程各自 listen 同一端口，由内核做负载均衡；注意它会让连接分布不均且难以做全局限流 |
| `TCP_NODELAY` | 1 | 关闭 Nagle 算法。Nagle 会攒小包，与对端延迟 ACK 叠加可能引入 40 ms 延迟，对 RPC/游戏是灾难 |
| `SO_KEEPALIVE` | 通常不开 | 默认 2 小时才探测，对业务无意义；改用应用层心跳（30 s 间隔 + 3 次超时） |
| `SO_LINGER` | 不设置 | 设为 0 会让 `close` 直接发 RST 丢弃数据；只在需要快速回收端口时谨慎使用 |
| `TCP_CORK` | 不用 | 用于攒大包减少小包，但会引入延迟；本库用应用层缓冲区合并写更可控 |
| `SO_RCVBUF/SO_SNDBUF` | 不手动设 | 内核自动调优（`tcp_moderate_rcvbuf`）通常优于固定值；手动设置会关闭自动调优 |

### 7.6 错误处理清单

| errno | 含义 | 处理 |
| --- | --- | --- |
| `EAGAIN` / `EWOULDBLOCK` | 非阻塞下无数据/缓冲满 | 正常，返回等待下次事件 |
| `EINTR` | 被信号中断 | 重试，不能当作错误 |
| `ECONNRESET` | 对端发 RST | 关闭连接，记录日志（常见于对端进程崩溃） |
| `EPIPE` | 写已关闭的连接 | 关闭连接；同时忽略 SIGPIPE |
| `ETIMEDOUT` | 重传超时 | 关闭连接 |
| `EMFILE` / `ENFILE` | fd 耗尽 | 预留 idle fd 技巧；同时触发告警，这是容量问题而非代码问题 |
| `ENOBUFS` / `ENOMEM` | 内核内存不足 | 退避重试，不能忙等 |

---

## 8. 应用层协议：解决粘包与拆包

### 8.1 粘包的本质

TCP 是**字节流**协议，不是消息协议。它只保证字节有序、可靠、不重复，**不保证"一次 write 对应一次 read"**。所谓"粘包"，在 TCP 层面根本不是一个问题——它只是"应用层没有定义消息边界"的表现。

具体成因：

1. **发送端合并**：Nagle 算法把多个小 write 合并成一个段发出。
2. **接收端合并**：接收缓冲区里累积了多个段，一次 `read` 全部读出。
3. **MSS 分片**：一个大于 MSS 的消息被拆成多个段，`read` 只能读到一部分。

所以"解决粘包"的准确表述是：**在字节流之上定义可解析的帧边界**。

### 8.2 三种成帧方案

| 方案 | 原理 | 优点 | 缺点 | 适用 |
| --- | --- | --- | --- | --- |
| 定长消息 | 每条消息固定 N 字节 | 解析最简单、可随机访问 | 消息长度不一时浪费带宽；长度上限写死；小消息填充严重 | 传感器采样、固定格式的行情快照 |
| 分隔符 | 用 `\r\n` 或 `\0` 分隔 | 可读性好、便于 telnet 调试 | 消息体内出现分隔符必须转义；需逐字节扫描；二进制不安全 | 文本协议（HTTP/1.1 头部、SMTP、Redis RESP） |
| 长度前缀 | 头部固定长度字段声明 body 长度 | O(1) 定位边界、二进制安全、无转义 | 需要定义头部格式；必须限制最大长度防攻击 | **二进制 RPC、IM、游戏、自定义协议** |
| TLV | Type-Length-Value 逐字段自描述 | 扩展性最好、可跳过未知字段 | 解析开销大、头部冗余 | Protobuf、Thrift 等 IDL 序列化 |

**本项目选择"固定头 + 长度前缀"**，理由是：

- 二进制安全：body 可以是任意字节，不需要转义，不需要扫描；
- 解析是 O(1)：拿到头部就知道要等多少字节，不需要边读边找分隔符；
- 与业务解耦：body 内部可以是 JSON、Protobuf 或自定义结构，网络层不关心；
- 可校验：固定头可以放 magic 与版本号，为协议演进留出空间。

### 8.3 协议头设计

![图 8-1 协议帧结构：16 字节固定头 + 变长 body](assets/protocol-frame.png)

| 偏移 | 长度 | 字段 | 说明 |
| --- | --- | --- | --- |
| 0 | 4 | magic | 固定 `0x4E455431`（ASCII "NET1"），用于快速识别非法连接与协议错位 |
| 4 | 1 | version | 协议版本，当前 1；用于灰度升级时兼容旧客户端 |
| 5 | 1 | type | 消息类型：1=请求、2=响应、3=心跳、4=推送、5=错误 |
| 6 | 2 | flags | 位标志：bit0 压缩、bit1 加密、bit2 是否带 CRC |
| 8 | 4 | seq | 请求序号，响应必须回填相同值，用于异步请求-响应匹配 |
| 12 | 4 | bodyLen | body 字节数，网络字节序，上限 8 MB |
| 16 | bodyLen | body | 业务载荷，由上层序列化决定 |

整头 16 字节，恰好是 4 的整数倍，无填充，可直接按偏移读取。

一个具体的帧示例（请求 `{"cmd":"ping"}`，body 14 字节），头部 16 字节按网络序展开为：

`4E 45 54 31` | `01` | `01` | `00 00` | `00 00 00 07` | `00 00 00 0E`

依次是 magic（NET1）、version=1、type=1、flags=0、seq=7、bodyLen=14，紧跟 14 字节 `{"cmd":"ping"}`。任何一段边界读错，都会在 magic 或长度校验处立刻暴露。

设计要点逐条解释：

1. **magic 放在最前**：如果一个 HTTP 请求打到这个端口，第一个 4 字节就对不上，可以立即断开并记录，而不是把它当成 bodyLen 解析出一个 10 亿字节的分配请求。
2. **version 单字节**：协议升级时，服务端可以按版本走不同解析器；新客户端连旧服务端时能给出明确错误，而不是神秘失败。
3. **seq 用于多路复用**：一个连接上可以并发多个请求，响应乱序返回时靠 seq 匹配。这是"连接复用"的基础，也是 HTTP/2 多路复用的同构思想。
4. **bodyLen 上限 8 MB**：这是**安全边界**，不是性能参数。没有上限时，攻击者发一个 `bodyLen = 0xFFFFFFFF` 的 16 字节包，服务端就会尝试 `resize(4GB)`，一次请求打爆内存。
5. **为什么不用结构体直接 `memcpy`**：

```cpp
// 错误示范
struct Header { uint32_t magic; uint8_t version; uint8_t type;
                uint16_t flags; uint32_t seq; uint32_t bodyLen; };
// sizeof(Header) 很可能是 20 而不是 16（对齐填充），
// 且跨大小端直接解释字段会得到错误值。
```

结构体内存布局受编译器对齐规则影响，`sizeof` 不等于字段长度之和；不同架构的字节序不同；从缓冲区直接 `reinterpret_cast` 还可能触发非对齐访问（在 ARM 上是总线错误，x86 上只是慢）。正确做法是**按字节偏移显式解析 + `ntohl` 转换**，并用 `static_assert` 固化头部长度常量。

### 8.4 解码状态机

解码器只需两个状态：等头部、等 body。半包到来时保持当前状态并返回，等下一次数据补齐。

```cpp
enum class State { kExpectHeader, kExpectBody, kBadFrame };

bool Codec::onMessage(Buffer* buf, std::vector<Message>* out) {
    for (;;) {
        if (state_ == State::kExpectHeader) {
            if (buf->readableBytes() < kHeaderLen) return true;   // 半包，等

            const char* p = buf->peek();
            uint32_t magic   = ntohl(*reinterpret_cast<const uint32_t*>(p));
            uint8_t  version = static_cast<uint8_t>(p[4]);
            uint8_t  type    = static_cast<uint8_t>(p[5]);
            uint32_t seq     = ntohl(*reinterpret_cast<const uint32_t*>(p + 8));
            uint32_t bodyLen = ntohl(*reinterpret_cast<const uint32_t*>(p + 12));

            if (magic != kMagic || version != kVersion || bodyLen > kMaxBody) {
                state_ = State::kBadFrame;                        // 立即断开，不做恢复
                return false;
            }
            buf->retrieve(kHeaderLen);                            // 无论如何都要消费头部
            cur_.type = type; cur_.seq = seq; cur_.bodyLen = bodyLen;
            state_ = State::kExpectBody;
            if (bodyLen == 0) {          // 空 body 是合法帧，必须立即产出，否则死循环
                out->push_back(cur_);
                state_ = State::kExpectHeader;
                continue;
            }
        }

        if (state_ == State::kExpectBody) {
            if (buf->readableBytes() < cur_.bodyLen) return true;  // 半包，等
            cur_.body.assign(buf->peek(), cur_.bodyLen);
            buf->retrieve(cur_.bodyLen);
            out->push_back(cur_);
            state_ = State::kExpectHeader;
            // 继续循环，处理粘在同一缓冲区里的下一条消息
        }
    }
}
```

> 上面的代码为便于阅读做了精简：完整实现把 `bodyLen` 单独存放在 `pendingBodyLen_`，并在头部解析时用 `memcpy` 而非 `reinterpret_cast` 避免非对齐访问，见 `src/Codec.cc`。

三个必须成立的约束：

- **每次都消费头部**：即使 bodyLen 为 0 也要 `retrieve(16)`，否则缓冲区不前进，循环变死循环。
- **半包时不消费**：数据不够就原样保留，等下次 `handleRead` 追加后再试。这就是"拆包"的全部实现。
- **解码循环而非递归**：粘包时一次可能解出几百条消息，递归会爆栈。

### 8.5 Buffer 的实现

```cpp
class Buffer {
public:
    size_t readableBytes() const { return writerIndex_ - readerIndex_; }
    size_t writableBytes() const { return buffer_.size() - writerIndex_; }
    size_t prependableBytes() const { return readerIndex_; }

    const char* peek() const { return begin() + readerIndex_; }

    void retrieve(size_t n) {
        readerIndex_ += n;
        if (readerIndex_ == writerIndex_) readerIndex_ = writerIndex_ = kCheapPrepend;
    }

    ssize_t readFd(int fd, int* savedErrno) {
        char extrabuf[65536];                       // 栈上溢出缓冲，避免频繁扩容
        struct iovec vec[2];
        size_t writable = writableBytes();
        vec[0].iov_base = begin() + writerIndex_;
        vec[0].iov_len  = writable;
        vec[1].iov_base = extrabuf;
        vec[1].iov_len  = sizeof extrabuf;

        const int iovcnt = (writable < sizeof extrabuf) ? 2 : 1;
        ssize_t n = ::readv(fd, vec, iovcnt);
        if (n < 0) { *savedErrno = errno; }
        else if (static_cast<size_t>(n) <= writable) { writerIndex_ += n; }
        else {                                       // 读到了 extrabuf 里
            writerIndex_ = buffer_.size();
            append(extrabuf, n - writable);
        }
        return n;
    }

private:
    std::vector<char> buffer_;      // 初始 capacity = 1KB
    size_t readerIndex_ = kCheapPrepend;   // 预留 8 字节，便于 prepend 长度字段
    size_t writerIndex_ = kCheapPrepend;
};
```

设计取舍：

| 决策 | 选择 | 理由 |
| --- | --- | --- |
| 连续内存 vs 链式块 | 连续 `vector` | 解码时可直接指针运算；链式块需处理跨块拷贝，代码复杂度高 |
| 读空后是否释放内存 | 不释放，索引复位 | 长连接会反复用到同样大小的缓冲，保留可避免反复 malloc/free |
| 扩容策略 | 翻倍，上限 64 MB | 摊还 O(1)；上限防止异常输入导致内存失控 |
| 栈上 64 KB 溢出缓冲 | 使用 `readv` | 一次系统调用同时填缓冲区和溢出区，减少一次 read 与一次扩容 |
| 预留 8 字节 prepend | 是 | 编码时需要在 body 前插入头部，有预留空间可避免整体搬移 |

### 8.6 协议安全

| 攻击面 | 风险 | 防护 |
| --- | --- | --- |
| 超大 bodyLen | 内存耗尽 | 8 MB 硬上限，超限直接断连并计数 |
| 慢速攻击（slowloris） | 连接长期占用不释放 | 读超时：连接建立后 N 秒内未发完整包则关闭 |
| 半包洪水 | 每个连接缓存半个大包 | 限制单连接输入缓冲上限，超过即断 |
| 心跳缺失 | 死连接占用 fd | 应用层心跳 + 定时器扫描空闲连接 |
| 非法 magic 扫描 | 端口探测 | 首包校验失败立即断开，并记录源 IP 做限速 |

### 8.7 为什么不用 HTTP / Protobuf / gRPC

- **HTTP/1.1**：文本解析、头部冗长（一个请求至少几百字节）、队头阻塞、无内建多路复用。它适合浏览器与通用 API，不适合高频小消息的内部通信。若需要 HTTP，应当在其上再包一层（这也是本库可作为 nginx 上游的原因）。
- **Protobuf**：解决的是**序列化**问题，不解决**成帧**问题。Protobuf 的 `writeDelimitedTo` 本质也是"长度前缀 + 消息"，与本项目的头部设计同构。引入它需要 `protoc` 编译期依赖与 IDL 维护，对本项目是过度设计；但 body 内部完全可以放 Protobuf，这是自然的演进方向。
- **gRPC**：在 HTTP/2 之上，自带多路复用、流控、超时、拦截器。功能强大但依赖庞大（HTTP/2 帧解析、HPACK、TLS），完全掩盖了"网络库"本身要展示的机制。
- **Redis RESP**：`\r\n` 分隔 + 类型前缀，适合人类可读的调试场景，但二进制 body 需要转义，解析需扫描。

> 结论：协议选型取决于"消息形态"与"调试需求"。内部高频二进制通信用长度前缀；对外 API 用 HTTP/JSON；跨语言 RPC 用 gRPC。没有普适最优，只有匹配。
