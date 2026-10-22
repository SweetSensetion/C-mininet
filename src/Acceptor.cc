#include "net/Acceptor.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>

#include "net/EventLoop.h"
#include "net/Logging.h"

namespace net {

Acceptor::Acceptor(EventLoop* loop, const InetAddress& listenAddr, bool reuseport)
    : loop_(loop),
      acceptSocket_(Socket::createNonblockingOrDie(listenAddr.family())),
      acceptChannel_(loop, acceptSocket_.fd()),
      listening_(false),
      idleFd_(::open("/dev/null", O_RDONLY | O_CLOEXEC)) {
    acceptSocket_.setReuseAddr(true);
    acceptSocket_.setReusePort(reuseport);
    acceptSocket_.bindAddress(listenAddr);
    acceptChannel_.setReadCallback([this](Timestamp) { handleRead(); });
}

Acceptor::~Acceptor() {
    acceptChannel_.disableAll();
    acceptChannel_.remove();
    if (idleFd_ >= 0) {
        ::close(idleFd_);
    }
}

void Acceptor::listen() {
    listening_ = true;
    acceptSocket_.listen();
    // 监听 fd 用 ET：只在"有新连接"这个边沿触发一次，
    // 因此 handleRead 必须循环 accept 直到 EAGAIN
    acceptChannel_.enableReading();
}

void Acceptor::handleRead() {
    InetAddress peerAddr;
    for (;;) {
        int connfd = acceptSocket_.accept(&peerAddr);
        if (connfd >= 0) {
            if (newConnectionCallback_) {
                // 所有权在此转移：TcpServer 负责后续 close/托管
                newConnectionCallback_(connfd, peerAddr);
            } else {
                ::close(connfd);
            }
            continue;   // 继续 accept，直到 EAGAIN
        }

        const int savedErrno = errno;
        if (savedErrno == EAGAIN || savedErrno == EWOULDBLOCK) {
            break;      // 这一批连接已经取完
        }
        if (savedErrno == EINTR) {
            continue;   // 被信号打断，重试
        }
        if (savedErrno == EMFILE) {
            // fd 耗尽：listen fd 仍然可读，若不处理会在这里无限触发（LT 下更糟）。
            // 技巧：关闭保留的 idle fd -> accept 一个连接 -> 立刻 close 它
            //      -> 重新打开 idle fd。这样能优雅地拒绝这一个连接而不卡死循环。
            LOG_ERROR << "accept failed: EMFILE, out of file descriptors";
            ::close(idleFd_);
            idleFd_ = ::accept(acceptSocket_.fd(), nullptr, nullptr);
            if (idleFd_ >= 0) {
                ::close(idleFd_);
            }
            idleFd_ = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
            break;
        }
        LOG_ERROR << "accept failed, errno=" << savedErrno;
        break;
    }
}

}  // namespace net
