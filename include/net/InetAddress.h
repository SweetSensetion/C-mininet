// InetAddress.h -- 地址封装
//
// 用 sockaddr_storage 做存储而不是 sockaddr_in，是为了同时容纳 IPv4 / IPv6，
// 且保证对齐（sockaddr_storage 自带最大对齐）。如果直接存 sockaddr_in，
// 想同时支持 IPv6 就得写两套重载或做危险的 reinterpret_cast。
#ifndef MINET_INETADDRESS_H
#define MINET_INETADDRESS_H

#include <netinet/in.h>
#include <sys/socket.h>

#include <cstdint>
#include <string>

namespace net {

class InetAddress {
public:
    // 构造监听地址：port=0 表示由内核分配；loopbackOnly 只绑 127.0.0.1
    explicit InetAddress(uint16_t port = 0, bool loopbackOnly = false, bool ipv6 = false);

    // 从点分十进制/IPv6 字符串构造
    InetAddress(const std::string& ip, uint16_t port, bool ipv6 = false);

    // 从 accept/getsockname 拿到的原始地址构造
    explicit InetAddress(const struct sockaddr* addr);

    sa_family_t family() const { return addr_.ss_family; }

    std::string toIp() const;
    std::string toIpPort() const;
    uint16_t toPort() const;

    const struct sockaddr* getSockAddr() const {
        return reinterpret_cast<const struct sockaddr*>(&addr_);
    }

    socklen_t getSockLen() const {
        return addr_.ss_family == AF_INET ? sizeof(struct sockaddr_in)
                                          : sizeof(struct sockaddr_in6);
    }

    void setSockAddrInet6(const struct sockaddr_in6& addr6);

    uint32_t ipNetEndian() const;
    uint16_t portNetEndian() const;

private:
    struct sockaddr_storage addr_;
};

}  // namespace net

#endif  // MINET_INETADDRESS_H
