#include "net/TcpServer.h"

#include <cstdio>
#include <vector>

#include "net/EventLoop.h"
#include "net/Logging.h"
#include "net/Socket.h"

namespace net {

TcpServer::TcpServer(EventLoop* loop, const InetAddress& listenAddr,
                     const std::string& nameArg, Option option)
    : loop_(loop),
      ipPort_(listenAddr.toIpPort()),
      name_(nameArg),
      acceptor_(new Acceptor(loop, listenAddr, option == kReusePort)),
      threadPool_(new EventLoopThreadPool(loop, nameArg)),
      started_(0),
      nextConnId_(1) {
    acceptor_->setNewConnectionCallback([this](int sockfd, const InetAddress& peer) {
        newConnection(sockfd, peer);
    });
}

TcpServer::~TcpServer() {
    // 先拷出连接表，避免持锁调用 runInLoop（可能反向等锁）
    std::vector<TcpConnectionPtr> conns;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        conns.reserve(connections_.size());
        for (const auto& kv : connections_) {
            conns.push_back(kv.second);
        }
        connections_.clear();
    }
    for (const TcpConnectionPtr& conn : conns) {
        EventLoop* ioLoop = conn->getLoop();
        ioLoop->runInLoop([conn]() { conn->connectDestroyed(); });
    }
}

void TcpServer::setThreadNum(int numThreads) {
    threadPool_->setThreadNum(numThreads);
}

void TcpServer::start() {
    // fetch_add 保证多线程重复调用 start() 时只有第一次真正生效
    if (started_.fetch_add(1) == 0) {
        threadPool_->start(threadInitCallback_);
        // listen 必须回到 baseLoop 线程执行，保证 acceptChannel_ 注册到正确的 epoll
        loop_->runInLoop([this]() { acceptor_->listen(); });
    }
}

void TcpServer::newConnection(int sockfd, const InetAddress& peerAddr) {
    loop_->assertInLoopThread();

    // 轮询选择 subReactor：连接在该线程内完成全部 IO，
    // 因此同一连接的读写天然串行，不需要任何锁。
    EventLoop* ioLoop = threadPool_->getNextLoop();

    char buf[64];
    snprintf(buf, sizeof buf, "#%d", nextConnId_);
    ++nextConnId_;
    std::string connName = name_ + buf;

    InetAddress localAddr(Socket::getLocalAddr(sockfd));

    TcpConnectionPtr conn(new TcpConnection(ioLoop, connName, sockfd, localAddr, peerAddr));
    {
        std::lock_guard<std::mutex> lock(mutex_);
        connections_[connName] = conn;
    }

    conn->setConnectionCallback(connectionCallback_);
    conn->setMessageCallback(messageCallback_);
    conn->setWriteCompleteCallback(writeCompleteCallback_);
    // 关闭回调由库内部接管：断开时把连接从表中摘除
    conn->setCloseCallback([this](const TcpConnectionPtr& c) { removeConnection(c); });

    // 必须投递到 IO 线程：对象创建、channel 注册、首次回调都在同一线程完成
    ioLoop->runInLoop([conn]() { conn->connectEstablished(); });
}

void TcpServer::removeConnection(const TcpConnectionPtr& conn) {
    // 这里可能已经在 IO 线程，也可能在业务线程；统一投递到 baseLoop
    loop_->runInLoop([this, conn]() { removeConnectionInLoop(conn); });
}

void TcpServer::removeConnectionInLoop(const TcpConnectionPtr& conn) {
    loop_->assertInLoopThread();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        connections_.erase(conn->name());
    }
    // 延迟到下一轮事件循环再销毁：当前调用栈还在该连接的 handleClose 里，
    // 立即销毁会让栈上的 this 悬空。
    EventLoop* ioLoop = conn->getLoop();
    ioLoop->queueInLoop([conn]() { conn->connectDestroyed(); });
}

}  // namespace net
