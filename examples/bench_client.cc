// bench_client.cc -- 压测客户端
//
// 策略：多线程 + 每线程多连接 + 流水线（pipeline）发送。
//   流水线是为了让"请求-响应"的 RTT 被重叠掉，测出的是服务器的处理吞吐，
//   而不是客户端一次往返的延迟。
//
// 用法：./bench_client [ip] [port] [threads] [conns_per_thread] [seconds] [body_size]
// 示例：./bench_client 127.0.0.1 9000 4 8 10 64
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "net/Protocol.h"

namespace {

const int kPipeline = 32;   // 每条连接同时在途的请求数

bool writeAll(int fd, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<size_t>(n);
        } else if (n < 0 && (errno == EINTR)) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

bool readAll(int fd, char* data, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t n = ::recv(fd, data + got, len - got, 0);
        if (n > 0) {
            got += static_cast<size_t>(n);
        } else if (n == 0) {
            return false;
        } else if (errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

int connectTo(const char* ip, uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, ip, &addr.sin_addr);

    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof addr) < 0) {
        ::close(fd);
        return -1;
    }
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}

}  // namespace

int main(int argc, char* argv[]) {
    const char* ip = (argc > 1) ? argv[1] : "127.0.0.1";
    uint16_t port = (argc > 2) ? static_cast<uint16_t>(atoi(argv[2])) : 9000;
    int threads = (argc > 3) ? atoi(argv[3]) : 4;
    int connsPerThread = (argc > 4) ? atoi(argv[4]) : 8;
    int seconds = (argc > 5) ? atoi(argv[5]) : 10;
    size_t bodySize = (argc > 6) ? static_cast<size_t>(atoi(argv[6])) : 64;

    std::string body(bodySize, 'x');
    std::string frame;
    {
        net::protocol::Message req;
        req.type = net::protocol::kRequest;
        req.seq = 1;
        req.body = body;
        net::Buffer buf;
        net::protocol::encode(req, &buf);
        frame.assign(buf.peek(), buf.readableBytes());
    }

    std::atomic<uint64_t> totalResponses(0);
    std::atomic<uint64_t> failedConnections(0);

    auto worker = [&](int tid) {
        std::vector<int> fds;
        for (int i = 0; i < connsPerThread; ++i) {
            int fd = connectTo(ip, port);
            if (fd < 0) {
                failedConnections.fetch_add(1);
                continue;
            }
            fds.push_back(fd);
        }
        if (fds.empty()) return;

        // 预热：每条连接灌满流水线
        for (int fd : fds) {
            for (int k = 0; k < kPipeline; ++k) {
                if (!writeAll(fd, frame.data(), frame.size())) break;
            }
        }

        const size_t respLen = net::protocol::kHeaderLen + bodySize;
        std::vector<char> respBuf(respLen);
        uint64_t local = 0;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);

        while (std::chrono::steady_clock::now() < deadline) {
            for (size_t i = 0; i < fds.size(); ++i) {
                int fd = fds[i];
                if (fd < 0) continue;   // 已断开的连接
                if (!readAll(fd, respBuf.data(), respLen)) {
                    failedConnections.fetch_add(1);
                    ::close(fd);
                    fds[i] = -1;
                    continue;
                }
                ++local;
                // 读完一个立刻补一个，保持流水线深度
                if (!writeAll(fd, frame.data(), frame.size())) {
                    failedConnections.fetch_add(1);
                    ::close(fd);
                    fds[i] = -1;
                }
            }
        }
        totalResponses.fetch_add(local);
        for (int fd : fds) {
            if (fd >= 0) ::close(fd);
        }
        (void)tid;
    };

    printf("bench: %s:%u threads=%d conns/thread=%d seconds=%d body=%zu pipeline=%d\n",
           ip, port, threads, connsPerThread, seconds, bodySize, kPipeline);

    auto begin = std::chrono::steady_clock::now();
    std::vector<std::thread> pool;
    pool.reserve(static_cast<size_t>(threads));
    for (int i = 0; i < threads; ++i) {
        pool.emplace_back(worker, i);
    }
    for (std::thread& t : pool) t.join();
    auto end = std::chrono::steady_clock::now();

    double elapsed = std::chrono::duration<double>(end - begin).count();
    uint64_t total = totalResponses.load();
    printf("responses=%llu elapsed=%.2fs qps=%.0f failed_conns=%llu\n",
           static_cast<unsigned long long>(total), elapsed,
           elapsed > 0 ? total / elapsed : 0.0,
           static_cast<unsigned long long>(failedConnections.load()));
    return 0;
}
