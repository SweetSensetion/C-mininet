# MiniNet —— 基于主从 Reactor 的 C++11 高性能网络库

一个从零实现的 Linux 网络库：epoll + 非阻塞 IO + one loop per thread + 有界线程池 + 长度前缀二进制协议。
不依赖任何第三方网络框架，只用 glibc、pthread 与 C++11 标准库。

[![ci](https://github.com/YOUR_NAME/YOUR_REPO/actions/workflows/ci.yml/badge.svg)](https://github.com/YOUR_NAME/YOUR_REPO/actions/workflows/ci.yml)

> 首次推送到 GitHub 后，把上面 badge 里的 `YOUR_NAME/YOUR_REPO` 换成实际仓库路径即可。
> CI 会做三件事：Release 构建、跑 `codec_test`、起真实服务器压 5 秒。

## 特性

| 能力 | 实现 |
| --- | --- |
| IO 多路复用 | epoll（ET + 非阻塞 fd），`Poller` 抽象支持替换后端 |
| 并发模型 | 主从 Reactor：1 个 mainReactor 负责 accept，N 个 subReactor 各跑一个 EventLoop |
| 线程安全 | 每连接线程封闭；跨线程通过 `runInLoop/queueInLoop` + eventfd 唤醒 |
| 定时器 | `std::set` 有序定时器 + `epoll_wait` 超时，支持周期任务与取消 |
| 缓冲区 | 连续内存 + 读写索引，`readv` 两段读，读空后索引复位避免反复分配 |
| 协议 | 16 字节固定头（magic/version/type/flags/seq/bodyLen）+ 变长 body |
| 生命周期 | `shared_ptr/weak_ptr` + `Channel::tie()`，杜绝回调期间 use-after-free |
| 背压 | 发送缓冲高水位回调 + 线程池有界队列 |

## 目录结构

```
include/net/     对外头文件（EventLoop、Channel、Poller、Buffer、TcpConnection ...）
src/             实现
examples/        echo_server（回显服务器）、bench_client（压测客户端）
tests/           codec_test（协议编解码单测，不依赖网络）
docs/            技术设计说明（Markdown + Word）、架构图
tools/           文档构建（Markdown→DOCX）、架构图生成、静态自检
```

## 文档

- [docs/技术设计说明.md](docs/技术设计说明.md)：完整技术说明，含选型论证、实现细节、面试问答
- [docs/高性能Reactor网络库技术设计说明.docx](docs/高性能Reactor网络库技术设计说明.docx)：排版后的 Word 版，可直接用于项目评审/面试材料
- 重新生成：`python tools/make_diagrams.py && python tools/build_doc.py`

## 构建与运行

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure

./build/echo_server 9000 4          # 端口 9000，4 个 IO 线程
./build/bench_client 127.0.0.1 9000 4 8 10 64
```

压测前记得调大 fd 上限：

```bash
ulimit -n 65535
```

## 实测数据

环境：WSL2 + Ubuntu 26.04.1（内核 6.18.54.1），16 vCPU，GCC 15.2.0，CMake 4.2.3，回环。

| 项目 | 结果 |
| --- | --- |
| 构建 | `-Wall -Wextra` 下 0 error / 0 warning |
| 单测 | `codec_test` 5 项全通过 |
| 吞吐 | 4 IO 线程、32 连接、流水线 32、64B body → **63~66 万 QPS**（两次实测），0 连接失败 |
| 并发连接 | 10,000 条长连接 → 服务端 **10,016 fd / 35 MB RSS / 5 线程**，断开后存活 |

> 回环压测中客户端与服务端共享 CPU，只反映处理效率，不代表线上容量。

辅助脚本（需在 WSL 中执行）：`tools/probe_wsl.sh`（环境检查）、`tools/build_wsl.sh`（构建 + 单测）、`tools/smoke_test.sh`（端到端）、`tools/conn_test.sh`（万连接）。

## 最小使用示例

```cpp
EventLoop loop;
TcpServer server(&loop, InetAddress(9000), "EchoServer");
server.setThreadNum(4);
server.setConnectionCallback([](const TcpConnectionPtr& conn) {
    if (conn->connected()) conn->setContext(std::make_shared<Codec>());
});
server.setMessageCallback([](const TcpConnectionPtr& conn, Buffer* buf, Timestamp) {
    auto codec = std::static_pointer_cast<Codec>(conn->getContext());
    std::vector<protocol::Message> msgs;
    if (!codec->onMessage(buf, &msgs)) { conn->forceClose(); return; }
    for (const auto& m : msgs) {
        protocol::Message resp; resp.type = protocol::kResponse; resp.seq = m.seq; resp.body = m.body;
        Buffer out; protocol::encode(resp, &out); conn->send(&out);
    }
});
server.start();
loop.loop();
```

## 关键设计一句话

- **epoll 而非 select/poll**：就绪事件 O(k) 而非 O(n)，无 1024 fd 上限，无每次全量拷贝。
- **主从而非单 Reactor**：accept 与 IO 解耦，IO 随核扩展，连接线程封闭所以无锁。
- **非阻塞 + ET**：事件驱动不空转，一次事件读干净，避免重复通知。
- **长度前缀而非分隔符**：O(1) 定位边界、二进制安全、无需转义扫描。
- **shared_ptr 而非裸指针**：连接生命周期跨线程、跨回调，必须引用计数 + `tie()` 兜底。

详细论证见 [docs/技术设计说明.md](docs/技术设计说明.md)。

## 已知限制

不做 TLS/HTTP、不做跨平台、不做多进程、不做零拷贝 sendfile。
这些是刻意排除的范围，理由见设计文档第 14 章。
