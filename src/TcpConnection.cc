#include "net/TcpConnection.h"

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "net/EventLoop.h"
#include "net/Logging.h"

namespace net {

TcpConnection::TcpConnection(EventLoop* loop, const std::string& nameArg, int sockfd,
                             const InetAddress& localAddr, const InetAddress& peerAddr)
    : loop_(loop),
      name_(nameArg),
      state_(kConnecting),
      reading_(true),
      socket_(new Socket(sockfd)),
      channel_(new Channel(loop, sockfd)),
      localAddr_(localAddr),
      peerAddr_(peerAddr),
      highWaterMark_(64 * 1024 * 1024) {
    channel_->setReadCallback([this](Timestamp t) { handleRead(t); });
    channel_->setWriteCallback([this]() { handleWrite(); });
    channel_->setCloseCallback([this]() { handleClose(); });
    channel_->setErrorCallback([this]() { handleError(); });
    // keepalive 交给应用层心跳（内核默认 2 小时才探测，对业务无意义），
    // 这里只关闭 Nagle：小包立即发出，避免 40ms 级别的延迟抖动。
    socket_->setKeepAlive(true);
    socket_->setTcpNoDelay(true);
}

TcpConnection::~TcpConnection() {
    LOG_DEBUG << "TcpConnection " << name_ << " destroyed, state=" << state_.load();
}

void TcpConnection::connectEstablished() {
    loop_->assertInLoopThread();
    setState(kConnected);
    // 关键：把自身 weak_ptr 交给 Channel。这样即便业务在回调里
    // 直接 reset 掉最后一个 shared_ptr，handleEvent 期间的 guard 也能保命。
    channel_->tie(shared_from_this());
    channel_->enableReading();
    if (connectionCallback_) {
        connectionCallback_(shared_from_this());
    }
}

void TcpConnection::connectDestroyed() {
    loop_->assertInLoopThread();
    if (state_ == kConnected) {
        setState(kDisconnected);
        channel_->disableAll();
        if (connectionCallback_) {
            connectionCallback_(shared_from_this());
        }
    }
    channel_->remove();
}

void TcpConnection::handleRead(Timestamp receiveTime) {
    loop_->assertInLoopThread();

    // ET 模式下必须一直读到 EAGAIN：内核只会通知"状态变化"一次，
    // 如果只读一次就返回，残留在内核缓冲区的数据将永远不会再触发事件。
    for (;;) {
        int savedErrno = 0;
        ssize_t n = inputBuffer_.readFd(channel_->fd(), &savedErrno);

        if (n > 0) {
            if (messageCallback_) {
                messageCallback_(shared_from_this(), &inputBuffer_, receiveTime);
            }
            // 回调里可能已关闭连接或 stopRead，必须检查后再继续读
            if (!reading_ || state_ != kConnected) {
                break;
            }
            continue;
        }

        if (n == 0) {
            // 对端有序关闭（FIN）
            if (reading_ && state_ == kConnected) {
                handleClose();
            }
            break;
        }

        if (savedErrno == EAGAIN || savedErrno == EWOULDBLOCK) {
            break;   // 读干净了，正常路径
        }
        if (savedErrno == EINTR) {
            continue;
        }
        errno = savedErrno;
        handleError();
        break;
    }
}

void TcpConnection::handleWrite() {
    loop_->assertInLoopThread();
    if (!channel_->isWriting()) {
        return;   // 已取消关注，属于迟到的事件
    }

    ssize_t n = ::write(channel_->fd(), outputBuffer_.peek(), outputBuffer_.readableBytes());
    if (n > 0) {
        outputBuffer_.retrieve(static_cast<size_t>(n));
        if (outputBuffer_.readableBytes() == 0) {
            // 写完必须立刻取消 EPOLLOUT：发送缓冲区"可写"是常态，
            // 持续关注会让 epoll_wait 立即返回，事件循环变成忙等。
            channel_->disableWriting();
            if (writeCompleteCallback_) {
                writeCompleteCallback_(shared_from_this());
            }
            if (state_ == kDisconnecting) {
                shutdownInLoop();
            }
        }
    } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        LOG_ERROR << "TcpConnection::handleWrite write failed, errno=" << errno;
        handleError();
    }
}

void TcpConnection::handleClose() {
    loop_->assertInLoopThread();
    if (state_ == kDisconnected) {
        return;   // 幂等：重复的关闭通知直接忽略
    }
    setState(kDisconnected);
    channel_->disableAll();

    TcpConnectionPtr guardThis(shared_from_this());
    if (connectionCallback_) {
        connectionCallback_(guardThis);
    }
    // 通知 TcpServer 把连接从 map 中摘掉（由它负责延迟销毁）
    if (closeCallback_) {
        closeCallback_(guardThis);
    }
}

void TcpConnection::handleError() {
    int err = Socket::getSocketError(channel_->fd());
    LOG_ERROR << "TcpConnection " << name_ << " error: " << strerror(err);
    handleClose();
}

void TcpConnection::send(const void* data, size_t len) {
    if (state_ != kConnected) {
        return;
    }
    if (loop_->isInLoopThread()) {
        sendInLoop(data, len);
    } else {
        // 跨线程发送：必须把数据拷贝进任务，并持有 shared_ptr 保活，
        // 否则任务执行前连接可能已经被销毁。
        std::string message(static_cast<const char*>(data), len);
        TcpConnectionPtr self = shared_from_this();
        loop_->runInLoop([self, message]() { self->sendInLoop(message.data(), message.size()); });
    }
}

void TcpConnection::send(const std::string& message) {
    send(message.data(), message.size());
}

void TcpConnection::send(Buffer* buf) {
    if (state_ != kConnected) {
        return;
    }
    if (loop_->isInLoopThread()) {
        sendInLoop(buf->peek(), buf->readableBytes());
        buf->retrieveAll();
    } else {
        std::string message = buf->retrieveAllAsString();
        TcpConnectionPtr self = shared_from_this();
        loop_->runInLoop([self, message]() { self->sendInLoop(message.data(), message.size()); });
    }
}

void TcpConnection::sendInLoop(const void* data, size_t len) {
    loop_->assertInLoopThread();
    if (state_ == kDisconnected) {
        LOG_WARN << "send on disconnected connection " << name_;
        return;
    }

    ssize_t nwrote = 0;
    size_t remaining = len;
    bool faultError = false;

    // 输出缓冲为空时先尝试直接写：省掉一次用户态拷贝，
    // 空载小包场景下这条路径命中率最高。
    if (!channel_->isWriting() && outputBuffer_.readableBytes() == 0) {
        nwrote = ::write(channel_->fd(), data, len);
        if (nwrote >= 0) {
            remaining = len - static_cast<size_t>(nwrote);
            if (remaining == 0 && writeCompleteCallback_) {
                writeCompleteCallback_(shared_from_this());
            }
        } else {
            nwrote = 0;
            if (errno != EWOULDBLOCK) {
                if (errno == EPIPE || errno == ECONNRESET) {
                    faultError = true;   // 对端已关闭，后续交给 handleClose
                }
            }
        }
    }

    if (faultError) {
        // 写失败且不是 EAGAIN：对端已关闭（EPIPE/ECONNRESET），
        // 继续缓存数据没有意义，直接进入关闭流程。
        handleClose();
        return;
    }

    if (remaining > 0) {
        size_t oldLen = outputBuffer_.readableBytes();
        // 高水位：积压超过阈值说明对端消费跟不上，业务应降级或断开，
        // 否则内存会被单个慢连接吃光。
        if (oldLen + remaining >= highWaterMark_ && oldLen < highWaterMark_ &&
            highWaterMarkCallback_) {
            highWaterMarkCallback_(shared_from_this(), oldLen + remaining);
        }
        outputBuffer_.append(static_cast<const char*>(data) + nwrote, remaining);
        if (!channel_->isWriting()) {
            channel_->enableWriting();
        }
    }
}

void TcpConnection::shutdown() {
    if (state_ == kConnected) {
        setState(kDisconnecting);
        TcpConnectionPtr self = shared_from_this();
        loop_->runInLoop([self]() { self->shutdownInLoop(); });
    }
}

void TcpConnection::shutdownInLoop() {
    loop_->assertInLoopThread();
    if (!channel_->isWriting()) {
        // 没有待发数据，直接关写端；read 侧随后会收到对端 FIN
        socket_->shutdownWrite();
    }
    // 还有数据在发：等 handleWrite 发现缓冲清空后再 shutdownWrite
}

void TcpConnection::forceClose() {
    if (state_ == kConnected || state_ == kDisconnecting) {
        setState(kDisconnecting);
        TcpConnectionPtr self = shared_from_this();
        loop_->runInLoop([self]() { self->forceCloseInLoop(); });
    }
}

void TcpConnection::forceCloseInLoop() {
    loop_->assertInLoopThread();
    if (state_ == kConnected || state_ == kDisconnecting) {
        handleClose();
    }
}

void TcpConnection::setTcpNoDelay(bool on) {
    socket_->setTcpNoDelay(on);
}

void TcpConnection::startRead() {
    TcpConnectionPtr self = shared_from_this();
    loop_->runInLoop([self]() {
        if (!self->reading_) {
            self->reading_ = true;
            self->channel_->enableReading();
        }
    });
}

void TcpConnection::stopRead() {
    TcpConnectionPtr self = shared_from_this();
    loop_->runInLoop([self]() {
        if (self->reading_) {
            self->reading_ = false;
            self->channel_->disableReading();
        }
    });
}

}  // namespace net
