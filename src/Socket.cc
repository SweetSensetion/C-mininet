#include "net/Socket.h"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>

#include <cstring>

#include "net/Logging.h"

namespace net {

Socket::~Socket() {
    if (sockfd_ >= 0) {
        ::close(sockfd_);
    }
}

void Socket::bindAddress(const InetAddress& localaddr) {
    int ret = ::bind(sockfd_, localaddr.getSockAddr(), localaddr.getSockLen());
    if (ret < 0) {
        LOG_FATAL << "bind failed, errno=" << errno << " addr=" << localaddr.toIpPort();
    }
}

void Socket::listen() {
    // SOMAXCONN 让内核使用 net.core.somaxconn 的值，不要写死 5 或 128
    int ret = ::listen(sockfd_, SOMAXCONN);
    if (ret < 0) {
        LOG_FATAL << "listen failed, errno=" << errno;
    }
}

int Socket::accept(InetAddress* peeraddr) {
    struct sockaddr_storage addr;
    memset(&addr, 0, sizeof addr);
    socklen_t len = sizeof addr;
    // accept4 直接带 NONBLOCK|CLOEXEC：省两次 fcntl，且无竞态窗口
    int connfd = ::accept4(sockfd_, reinterpret_cast<struct sockaddr*>(&addr), &len,
                           SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (connfd >= 0 && peeraddr) {
        *peeraddr = InetAddress(reinterpret_cast<const struct sockaddr*>(&addr));
    }
    return connfd;
}

void Socket::shutdownWrite() {
    // 只关写端，读端仍可收数据：这是"优雅关闭"的关键，
    // 直接 close 会丢掉对端还没读完的数据并触发 RST。
    if (::shutdown(sockfd_, SHUT_WR) < 0) {
        LOG_DEBUG << "shutdownWrite failed, errno=" << errno;
    }
}

void Socket::setTcpNoDelay(bool on) {
    int optval = on ? 1 : 0;
    ::setsockopt(sockfd_, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof optval);
}

void Socket::setReuseAddr(bool on) {
    int optval = on ? 1 : 0;
    ::setsockopt(sockfd_, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof optval);
}

void Socket::setReusePort(bool on) {
    int optval = on ? 1 : 0;
    int ret = ::setsockopt(sockfd_, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof optval);
    if (ret < 0 && on) {
        LOG_ERROR << "SO_REUSEPORT not supported, errno=" << errno;
    }
}

void Socket::setKeepAlive(bool on) {
    int optval = on ? 1 : 0;
    ::setsockopt(sockfd_, SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof optval);
}

int Socket::createNonblockingOrDie(sa_family_t family) {
    int sockfd = ::socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
    if (sockfd < 0) {
        LOG_FATAL << "socket() failed, errno=" << errno;
    }
    return sockfd;
}

InetAddress Socket::getLocalAddr(int sockfd) {
    struct sockaddr_storage addr;
    memset(&addr, 0, sizeof addr);
    socklen_t len = sizeof addr;
    if (::getsockname(sockfd, reinterpret_cast<struct sockaddr*>(&addr), &len) < 0) {
        LOG_ERROR << "getsockname failed, errno=" << errno;
    }
    return InetAddress(reinterpret_cast<const struct sockaddr*>(&addr));
}

InetAddress Socket::getPeerAddr(int sockfd) {
    struct sockaddr_storage addr;
    memset(&addr, 0, sizeof addr);
    socklen_t len = sizeof addr;
    if (::getpeername(sockfd, reinterpret_cast<struct sockaddr*>(&addr), &len) < 0) {
        LOG_ERROR << "getpeername failed, errno=" << errno;
    }
    return InetAddress(reinterpret_cast<const struct sockaddr*>(&addr));
}

int Socket::getSocketError(int sockfd) {
    int optval = 0;
    socklen_t optlen = sizeof optval;
    if (::getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &optval, &optlen) < 0) {
        return errno;
    }
    return optval;
}

}  // namespace net
