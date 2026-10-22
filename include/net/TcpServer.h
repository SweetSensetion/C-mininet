// TcpServer.h -- 服务器门面
//
// 职责：持有 Acceptor 与 subReactor 线程池，把新连接分配给某个 IO 线程，
// 并维护连接表（name -> TcpConnection）以便统一关闭。
#ifndef MINET_TCPSERVER_H
#define MINET_TCPSERVER_H

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "net/Acceptor.h"
#include "net/Callbacks.h"
#include "net/EventLoopThreadPool.h"
#include "net/TcpConnection.h"

namespace net {

class EventLoop;

class TcpServer {
public:
    using ThreadInitCallback = std::function<void(EventLoop*)>;
    enum Option { kNoReusePort, kReusePort };

    TcpServer(EventLoop* loop, const InetAddress& listenAddr, const std::string& nameArg,
              Option option = kNoReusePort);
    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    void setThreadInitCallback(const ThreadInitCallback& cb) { threadInitCallback_ = cb; }
    void setConnectionCallback(const ConnectionCallback& cb) { connectionCallback_ = cb; }
    void setMessageCallback(const MessageCallback& cb) { messageCallback_ = cb; }
    void setWriteCompleteCallback(const WriteCompleteCallback& cb) { writeCompleteCallback_ = cb; }

    // 0 表示所有 IO 都在 baseLoop（单 Reactor）；>0 则启动对应数量的 subReactor
    void setThreadNum(int numThreads);

    void start();

    EventLoop* getLoop() const { return loop_; }
    const std::string& name() const { return name_; }

private:
    void newConnection(int sockfd, const InetAddress& peerAddr);
    void removeConnection(const TcpConnectionPtr& conn);
    void removeConnectionInLoop(const TcpConnectionPtr& conn);

    using ConnectionMap = std::map<std::string, TcpConnectionPtr>;

    EventLoop* loop_;
    const std::string ipPort_;
    const std::string name_;
    std::unique_ptr<Acceptor> acceptor_;
    std::shared_ptr<EventLoopThreadPool> threadPool_;

    ConnectionCallback connectionCallback_;
    MessageCallback messageCallback_;
    WriteCompleteCallback writeCompleteCallback_;
    ThreadInitCallback threadInitCallback_;

    std::atomic<int> started_;
    int nextConnId_;                 // 只在 baseLoop 线程访问
    mutable std::mutex mutex_;       // 保护 connections_（跨 IO 线程读写）
    ConnectionMap connections_;
};

}  // namespace net

#endif  // MINET_TCPSERVER_H
