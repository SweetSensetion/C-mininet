// Codec.h -- 协议解码器（状态机）
//
// 只做两件事：从 Buffer 里切出完整消息、把半包留在 Buffer 里等下次。
// 解码器本身不持有连接、不发送数据，因此可以脱离网络单测。
#ifndef MINET_CODEC_H
#define MINET_CODEC_H

#include <cstdint>
#include <vector>

#include "net/Buffer.h"
#include "net/Protocol.h"

namespace net {

class Codec {
public:
    explicit Codec(uint32_t maxBodyLen = protocol::kMaxBodyLen)
        : state_(kExpectHeader), maxBodyLen_(maxBodyLen), pendingBodyLen_(0) {}

    // 返回 false 表示帧非法（magic/version/长度越界），调用方应立即断开连接。
    // 返回 true 时 out 里可能有 0 条或多条消息（粘包）。
    bool onMessage(Buffer* buf, std::vector<protocol::Message>* out);

    void reset() {
        state_ = kExpectHeader;
        pendingBodyLen_ = 0;
        current_ = protocol::Message();
    }

private:
    enum State { kExpectHeader, kExpectBody };

    State state_;
    uint32_t maxBodyLen_;
    uint32_t pendingBodyLen_;
    protocol::Message current_;
};

}  // namespace net

#endif  // MINET_CODEC_H
