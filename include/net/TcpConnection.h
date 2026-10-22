// TcpConnection.h -- 一条已建立的 TCP 连接
//
// 生命周期由 shared_ptr 管理，但"归属"严格绑定在一个 EventLoop 上：
// 所有成员函数（除线程安全的 send 外）都只能在所属 loop 线程调用。
//
// 状态机：
//   kConnecting ──connectEstablished──> kConnected
//        │                                  │
//        │                          shutdown│
//        │                                  ▼
//        │                          kDisconnecting
//        │                                  │ 写缓冲清空
//        └──────────────┬───────────────────┘
//                       ▼
//                 kDisconnected
#ifndef MINET_TCPCONNECTION_H
#define MINET_TCPCONNECTION_H

#include <atomic>
#include <memory>
#include <string>

#include "net/Buffer.h"
#include "net/Callbacks.h"
#include "net/Channel.h"
#include "net/InetAddress.h"
#include "net/Socket.h"

namespace net {

class EventLoop;

class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
public:
    enum StateE { kDisconnected, kConnecting, kConnected, kDisconnecting };

    TcpConnection(EventLoop* loop, const std::string& nameArg, int sockfd,
                  const InetAddress& localAddr, const InetAddress& peerAddr);
    ~TcpConnection();

    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    EventLoop* getLoop() const { return loop_; }
    const std::string& name() const { return name_; }
    const InetAddress& localAddress() const { return localAddr_; }
    const InetAddress& peerAddress() const { return peerAddr_; }
    bool connected() const { return state_ == kConnected; }

    // 线程安全：可在任意线程调用，内部会投递到 IO 线程
    void send(const void* message, size_t len);
    void send(const std::string& message);
    void send(Buffer* message);

    // 优雅关闭：先 shutdown(SHUT_WR)，写完剩余数据再 close
    void shutdown();
    void forceClose();

    void setTcpNoDelay(bool on);

    void startRead();
    void stopRead();

    void setConnectionCallback(const ConnectionCallback& cb) { connectionCallback_ = cb; }
    void setMessageCallback(const MessageCallback& cb) { messageCallback_ = cb; }
    void setWriteCompleteCallback(const WriteCompleteCallback& cb) { writeCompleteCallback_ = cb; }
    void setHighWaterMarkCallback(const HighWaterMarkCallback& cb, size_t highWaterMark) {
        highWaterMarkCallback_ = cb;
        highWaterMark_ = highWaterMark;
    }
    void setCloseCallback(const CloseCallback& cb) { closeCallback_ = cb; }

    // 由 TcpServer 调用：连接建立完成 / 连接销毁
    void connectEstablished();
    void connectDestroyed();

    Buffer* inputBuffer() { return &inputBuffer_; }
    Buffer* outputBuffer() { return &outputBuffer_; }

    // 业务上下文：库不关心内容，用来给每条连接挂编解码器 / 会话状态。
    // 用 shared_ptr<void> 而不是 void*，是为了让上下文跟随连接一起析构。
    void setContext(const std::shared_ptr<void>& context) { context_ = context; }
    const std::shared_ptr<void>& getContext() const { return context_; }

private:
    void handleRead(Timestamp receiveTime);
    void handleWrite();
    void handleClose();
    void handleError();

    void sendInLoop(const void* data, size_t len);
    void shutdownInLoop();
    void forceCloseInLoop();
    void setState(StateE s) { state_ = s; }

    EventLoop* loop_;
    const std::string name_;
    std::atomic<StateE> state_;   // 只写于 IO 线程，但 connected() 可能被其他线程读
    bool reading_;

    std::unique_ptr<Socket> socket_;
    std::unique_ptr<Channel> channel_;

    const InetAddress localAddr_;
    const InetAddress peerAddr_;

    ConnectionCallback connectionCallback_;
    MessageCallback messageCallback_;
    WriteCompleteCallback writeCompleteCallback_;
    HighWaterMarkCallback highWaterMarkCallback_;
    size_t highWaterMark_;
    CloseCallback closeCallback_;

    Buffer inputBuffer_;    // 解码器消费
    Buffer outputBuffer_;   // 部分写时缓存剩余数据

    std::shared_ptr<void> context_;   // 业务自定义数据（如每连接 Codec）
};

}  // namespace net

#endif  // MINET_TCPCONNECTION_H
