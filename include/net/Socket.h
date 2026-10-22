// Socket.h -- fd 的 RAII 封装
//
// 核心思想：fd 是稀缺资源，必须"谁持有谁关闭"。用析构函数保证任何返回路径
// （包括异常、提前 return）都会 close，避免 fd 泄漏——这是服务器跑几天后
// "Too many open files" 最常见的根因。
#ifndef MINET_SOCKET_H
#define MINET_SOCKET_H

#include <sys/socket.h>

#include "net/InetAddress.h"

namespace net {

class Socket {
public:
    explicit Socket(int sockfd) : sockfd_(sockfd) {}
    ~Socket();

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    int fd() const { return sockfd_; }

    void bindAddress(const InetAddress& localaddr);
    void listen();
    // 返回已连接 fd（非阻塞 + CLOEXEC）；失败返回 -1 并设置 errno
    int accept(InetAddress* peeraddr);

    void shutdownWrite();

    void setTcpNoDelay(bool on);
    void setReuseAddr(bool on);
    void setReusePort(bool on);
    void setKeepAlive(bool on);

    // 创建非阻塞 socket。SOCK_NONBLOCK|SOCK_CLOEXEC 在 socket() 里一次到位：
    // 没有"创建后未设置标志"的时间窗口，也不会被 fork 出的子进程继承。
    static int createNonblockingOrDie(sa_family_t family);

    static InetAddress getLocalAddr(int sockfd);
    static InetAddress getPeerAddr(int sockfd);
    static int getSocketError(int sockfd);

private:
    const int sockfd_;
};

}  // namespace net

#endif  // MINET_SOCKET_H
