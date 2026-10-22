// Protocol.h -- 自定义二进制应用层协议
//
// 为什么需要它：TCP 是字节流，没有消息边界。必须由应用层定义"一条消息从哪
// 里开始、到哪里结束"，否则接收端无法区分"两个小包"和"一个大包"。
//
// 帧格式（固定 16 字节头 + 变长 body，头部恰好 4 字节对齐、无填充）：
//
//   0               1               2               3
//   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//   +-------------------------------+-------+-------+---------------+
//   |           magic (4)           | ver(1)| type(1)|   flags (2)   |
//   +-------------------------------+-------+-------+---------------+
//   |             seq (4)           |           bodyLen (4)         |
//   +-------------------------------+-------------------------------+
//   |                        body (bodyLen 字节)                    |
//   +---------------------------------------------------------------+
//
// 字段说明：
//   magic   0x4E455431 ("NET1")，用于识别非法连接 / 协议错位
//   version 协议版本，支持灰度升级
//   type    消息类型（请求/响应/心跳/推送）
//   flags   位标志（压缩、加密、是否带校验和）
//   seq     请求序号，响应回填同一值，用于并发请求匹配
//   bodyLen body 长度，必须设上限，否则是内存耗尽的攻击面
#ifndef MINET_PROTOCOL_H
#define MINET_PROTOCOL_H

#include <arpa/inet.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "net/Buffer.h"

namespace net {
namespace protocol {

const uint32_t kMagic = 0x4E455431u;   // "NET1"
const uint8_t kVersion = 1;
const size_t kHeaderLen = 16;
const uint32_t kMaxBodyLen = 8 * 1024 * 1024;   // 8MB 硬上限

enum MessageType : uint8_t {
    kRequest = 1,
    kResponse = 2,
    kHeartbeat = 3,
    kPush = 4,
    kError = 5,
};

struct Message {
    uint8_t type;
    uint16_t flags;
    uint32_t seq;
    std::string body;

    Message() : type(kRequest), flags(0), seq(0), body() {}
};

// 编码：头部按网络字节序逐字段写入，绝不 memcpy 整个结构体
// （结构体会被编译器填充，且大小端不同）。
inline void encode(const Message& msg, Buffer* out) {
    char header[kHeaderLen];
    uint32_t magic = htonl(kMagic);
    std::memcpy(header, &magic, 4);

    header[4] = static_cast<char>(kVersion);
    header[5] = static_cast<char>(msg.type);

    uint16_t flags = htons(msg.flags);
    std::memcpy(header + 6, &flags, 2);

    uint32_t seq = htonl(msg.seq);
    std::memcpy(header + 8, &seq, 4);

    uint32_t bodyLen = htonl(static_cast<uint32_t>(msg.body.size()));
    std::memcpy(header + 12, &bodyLen, 4);

    out->append(header, kHeaderLen);
    out->append(msg.body);
}

}  // namespace protocol
}  // namespace net

#endif  // MINET_PROTOCOL_H
