#include "net/Codec.h"

#include <arpa/inet.h>

#include <cstring>

namespace net {

bool Codec::onMessage(Buffer* buf, std::vector<protocol::Message>* out) {
    for (;;) {
        if (state_ == kExpectHeader) {
            // 半包：头部都没收全，原样留在 Buffer 里
            if (buf->readableBytes() < protocol::kHeaderLen) {
                return true;
            }

            const char* p = buf->peek();
            uint32_t magic = 0;
            uint32_t seq = 0;
            uint32_t bodyLen = 0;
            uint16_t flags = 0;

            // 用 memcpy 而不是 reinterpret_cast：x86 容忍非对齐访问但会变慢，
            // ARM 上直接触发总线错误；memcpy 会被编译器优化成单条指令。
            std::memcpy(&magic, p, 4);
            magic = ntohl(magic);
            const uint8_t version = static_cast<uint8_t>(p[4]);
            const uint8_t type = static_cast<uint8_t>(p[5]);
            std::memcpy(&flags, p + 6, 2);
            flags = ntohs(flags);
            std::memcpy(&seq, p + 8, 4);
            seq = ntohl(seq);
            std::memcpy(&bodyLen, p + 12, 4);
            bodyLen = ntohl(bodyLen);

            // 任何一项不合法都直接判定为坏帧：不做"尝试恢复"，
            // 因为后续字节的边界已经不可信，继续解析只会放大问题。
            if (magic != protocol::kMagic || version != protocol::kVersion ||
                bodyLen > maxBodyLen_) {
                return false;
            }

            // 无论 bodyLen 是否为 0，都必须消费头部，否则缓冲区不前进 -> 死循环
            buf->retrieve(protocol::kHeaderLen);

            current_.type = type;
            current_.flags = flags;
            current_.seq = seq;

            if (bodyLen == 0) {
                current_.body.clear();
                out->push_back(current_);
                continue;
            }

            pendingBodyLen_ = bodyLen;
            state_ = kExpectBody;
        }

        if (state_ == kExpectBody) {
            if (buf->readableBytes() < pendingBodyLen_) {
                return true;   // body 还没收全
            }
            current_.body.assign(buf->peek(), pendingBodyLen_);
            buf->retrieve(pendingBodyLen_);
            out->push_back(current_);
            state_ = kExpectHeader;
            // 继续循环：一次 read 可能带来多条消息（粘包），必须全部解出来
        }
    }
}

}  // namespace net
