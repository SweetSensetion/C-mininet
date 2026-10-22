#include "net/InetAddress.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <cstring>

namespace net {

namespace {

void fromIpPort(const char* ip, uint16_t port, struct sockaddr_in* addr) {
    addr->sin_family = AF_INET;
    addr->sin_port = htons(port);
    if (::inet_pton(AF_INET, ip, &addr->sin_addr) <= 0) {
        addr->sin_addr.s_addr = htonl(INADDR_ANY);
    }
}

void fromIpPort(const char* ip, uint16_t port, struct sockaddr_in6* addr) {
    addr->sin6_family = AF_INET6;
    addr->sin6_port = htons(port);
    if (::inet_pton(AF_INET6, ip, &addr->sin6_addr) <= 0) {
        addr->sin6_addr = in6addr_any;
    }
}

const struct sockaddr_in* sockaddr_in_cast(const struct sockaddr* addr) {
    return reinterpret_cast<const struct sockaddr_in*>(addr);
}

const struct sockaddr_in6* sockaddr_in6_cast(const struct sockaddr* addr) {
    return reinterpret_cast<const struct sockaddr_in6*>(addr);
}

}  // namespace

InetAddress::InetAddress(uint16_t port, bool loopbackOnly, bool ipv6) {
    memset(&addr_, 0, sizeof addr_);
    if (ipv6) {
        struct sockaddr_in6 addr6;
        memset(&addr6, 0, sizeof addr6);
        addr6.sin6_family = AF_INET6;
        addr6.sin6_addr = loopbackOnly ? in6addr_loopback : in6addr_any;
        addr6.sin6_port = htons(port);
        memcpy(&addr_, &addr6, sizeof addr6);
    } else {
        struct sockaddr_in addr4;
        memset(&addr4, 0, sizeof addr4);
        addr4.sin_family = AF_INET;
        addr4.sin_addr.s_addr = htonl(loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY);
        addr4.sin_port = htons(port);
        memcpy(&addr_, &addr4, sizeof addr4);
    }
}

InetAddress::InetAddress(const std::string& ip, uint16_t port, bool ipv6) {
    memset(&addr_, 0, sizeof addr_);
    if (ipv6) {
        struct sockaddr_in6 addr6;
        memset(&addr6, 0, sizeof addr6);
        fromIpPort(ip.c_str(), port, &addr6);
        memcpy(&addr_, &addr6, sizeof addr6);
    } else {
        struct sockaddr_in addr4;
        memset(&addr4, 0, sizeof addr4);
        fromIpPort(ip.c_str(), port, &addr4);
        memcpy(&addr_, &addr4, sizeof addr4);
    }
}

InetAddress::InetAddress(const struct sockaddr* addr) {
    memset(&addr_, 0, sizeof addr_);
    if (addr->sa_family == AF_INET) {
        memcpy(&addr_, addr, sizeof(struct sockaddr_in));
    } else if (addr->sa_family == AF_INET6) {
        memcpy(&addr_, addr, sizeof(struct sockaddr_in6));
    }
}

void InetAddress::setSockAddrInet6(const struct sockaddr_in6& addr6) {
    memset(&addr_, 0, sizeof addr_);
    memcpy(&addr_, &addr6, sizeof addr6);
}

std::string InetAddress::toIp() const {
    char buf[INET6_ADDRSTRLEN] = {0};
    if (addr_.ss_family == AF_INET) {
        ::inet_ntop(AF_INET, &sockaddr_in_cast(getSockAddr())->sin_addr, buf, sizeof buf);
    } else if (addr_.ss_family == AF_INET6) {
        ::inet_ntop(AF_INET6, &sockaddr_in6_cast(getSockAddr())->sin6_addr, buf, sizeof buf);
    }
    return buf;
}

std::string InetAddress::toIpPort() const {
    // 用 getnameinfo 统一处理 v4/v6，避免手写端口拼接
    char host[NI_MAXHOST] = {0};
    char serv[NI_MAXSERV] = {0};
    if (::getnameinfo(getSockAddr(), getSockLen(), host, sizeof host, serv, sizeof serv,
                      NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
        return "0.0.0.0:0";
    }
    std::string result(host);
    result.push_back(':');
    result.append(serv);
    return result;
}

uint16_t InetAddress::toPort() const {
    if (addr_.ss_family == AF_INET) {
        return ntohs(sockaddr_in_cast(getSockAddr())->sin_port);
    }
    if (addr_.ss_family == AF_INET6) {
        return ntohs(sockaddr_in6_cast(getSockAddr())->sin6_port);
    }
    return 0;
}

uint32_t InetAddress::ipNetEndian() const {
    if (addr_.ss_family == AF_INET) {
        return sockaddr_in_cast(getSockAddr())->sin_addr.s_addr;
    }
    return 0;
}

uint16_t InetAddress::portNetEndian() const {
    if (addr_.ss_family == AF_INET) {
        return sockaddr_in_cast(getSockAddr())->sin_port;
    }
    if (addr_.ss_family == AF_INET6) {
        return sockaddr_in6_cast(getSockAddr())->sin6_port;
    }
    return 0;
}

}  // namespace net
