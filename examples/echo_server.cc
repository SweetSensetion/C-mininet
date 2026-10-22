// echo_server.cc -- 示例服务器
//
// 功能：按自定义二进制协议收包，原样回显（type 改成 kResponse，seq 保持不变）。
// 编译：cmake -B build && cmake --build build -j
// 运行：./build/echo_server 9000 4
#include <signal.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

#include "net/Buffer.h"
#include "net/Codec.h"
#include "net/EventLoop.h"
#include "net/Logging.h"
#include "net/Protocol.h"
#include "net/TcpServer.h"

using namespace net;

namespace {

// 每条连接一个 Codec：解码器有状态（等头部/等 body），不能多连接共享
void onConnection(const TcpConnectionPtr& conn) {
    if (conn->connected()) {
        conn->setContext(std::make_shared<Codec>());
        LOG_INFO << "connection up: " << conn->name() << " from " << conn->peerAddress().toIpPort();
    } else {
        LOG_INFO << "connection down: " << conn->name();
    }
}

void onMessage(const TcpConnectionPtr& conn, Buffer* buf, Timestamp) {
    std::shared_ptr<Codec> codec = std::static_pointer_cast<Codec>(conn->getContext());
    if (!codec) {
        conn->forceClose();
        return;
    }

    std::vector<protocol::Message> messages;
    if (!codec->onMessage(buf, &messages)) {
        // 坏帧：协议已不可信，立即断开，避免继续解析垃圾数据
        LOG_WARN << "bad frame from " << conn->peerAddress().toIpPort() << ", closing";
        conn->forceClose();
        return;
    }

    // 一次 read 可能解出多条（粘包），逐条回显
    for (const protocol::Message& msg : messages) {
        protocol::Message resp;
        resp.type = protocol::kResponse;
        resp.flags = msg.flags;
        resp.seq = msg.seq;
        resp.body = msg.body;

        Buffer out;
        protocol::encode(resp, &out);
        conn->send(&out);   // send 内部处理部分写与跨线程投递
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    uint16_t port = 9000;
    int threads = 4;
    if (argc > 1) port = static_cast<uint16_t>(atoi(argv[1]));
    if (argc > 2) threads = atoi(argv[2]);

    // 向已关闭连接写数据会触发 SIGPIPE，默认动作是杀进程，必须忽略
    ::signal(SIGPIPE, SIG_IGN);
    Logger::setLogLevel(Logger::INFO);

    EventLoop loop;
    InetAddress listenAddr(port);
    TcpServer server(&loop, listenAddr, "EchoServer");
    server.setThreadNum(threads);
    server.setConnectionCallback(onConnection);
    server.setMessageCallback(onMessage);

    server.start();
    LOG_INFO << "echo server listening on " << listenAddr.toIpPort()
             << ", io threads = " << threads;
    loop.loop();
    return 0;
}
