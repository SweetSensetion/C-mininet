// codec_test.cc -- 协议编解码单元测试（不依赖网络）
//
// 覆盖四个最容易出错的点：
//   1. 逐字节喂入（最极端的拆包）能否正确还原；
//   2. 一次喂入多个消息（最极端的粘包）能否全部解出；
//   3. 零长度 body 是否会死循环；
//   4. 非法 magic / 超长 body 是否被拒绝。
#undef NDEBUG   // Release 构建会定义 NDEBUG 关掉 assert，测试里必须强制打开
#include <arpa/inet.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

#include "net/Buffer.h"
#include "net/Codec.h"
#include "net/Protocol.h"

using namespace net;

namespace {

std::string encodeOne(uint32_t seq, const std::string& body) {
    protocol::Message msg;
    msg.type = protocol::kRequest;
    msg.seq = seq;
    msg.body = body;
    Buffer buf;
    protocol::encode(msg, &buf);
    return std::string(buf.peek(), buf.readableBytes());
}

void testByteByByte() {
    const std::string frame = encodeOne(42, "hello world");
    Buffer buf;
    Codec codec;
    std::vector<protocol::Message> out;

    for (size_t i = 0; i < frame.size(); ++i) {
        buf.append(&frame[i], 1);
        bool ok = codec.onMessage(&buf, &out);
        assert(ok);
    }
    assert(out.size() == 1);
    assert(out[0].seq == 42);
    assert(out[0].body == "hello world");
    assert(buf.readableBytes() == 0);
    printf("[ok] byte-by-byte decode\n");
}

void testStickyPackets() {
    std::string all;
    all += encodeOne(1, "aaa");
    all += encodeOne(2, "");
    all += encodeOne(3, "ccc");

    Buffer buf;
    buf.append(all);
    Codec codec;
    std::vector<protocol::Message> out;
    bool ok = codec.onMessage(&buf, &out);

    assert(ok);
    assert(out.size() == 3);
    assert(out[0].seq == 1 && out[0].body == "aaa");
    assert(out[1].seq == 2 && out[1].body.empty());   // 零长度 body 不能卡住
    assert(out[2].seq == 3 && out[2].body == "ccc");
    assert(buf.readableBytes() == 0);
    printf("[ok] sticky packets + empty body\n");
}

void testBadMagic() {
    std::string frame = encodeOne(7, "data");
    frame[0] = 0x00;   // 破坏 magic

    Buffer buf;
    buf.append(frame);
    Codec codec;
    std::vector<protocol::Message> out;
    assert(codec.onMessage(&buf, &out) == false);
    printf("[ok] bad magic rejected\n");
}

void testOversizeBody() {
    std::string frame = encodeOne(8, "data");
    // 手工把 bodyLen 改成 0x7FFFFFFF
    uint32_t huge = htonl(0x7FFFFFFFu);
    memcpy(&frame[12], &huge, 4);

    Buffer buf;
    buf.append(frame);
    Codec codec;
    std::vector<protocol::Message> out;
    assert(codec.onMessage(&buf, &out) == false);
    printf("[ok] oversize body rejected\n");
}

}  // namespace

int main() {
    testByteByByte();
    testStickyPackets();
    testBadMagic();
    testOversizeBody();
    printf("all codec tests passed\n");
    return 0;
}
