#include "net/Channel.h"

#include "net/EventLoop.h"
#include "net/Logging.h"

namespace net {

const int Channel::kNoneEvent = 0;
// EPOLLPRI 是带外数据（TCP urgent data），罕见但一旦忽略会导致对端异常
const int Channel::kReadEvent = EPOLLIN | EPOLLPRI;
const int Channel::kWriteEvent = EPOLLOUT;

Channel::Channel(EventLoop* loop, int fd)
    : loop_(loop),
      fd_(fd),
      events_(0),
      revents_(0),
      index_(-1),
      addedToLoop_(false),
      eventHandling_(false),
      tied_(false) {}

Channel::~Channel() {
    // 正常流程下 remove() 已经把 fd 从 epoll 摘除；这里只做断言式提醒
    if (addedToLoop_) {
        LOG_ERROR << "Channel fd=" << fd_ << " destroyed while still in epoll";
    }
}

void Channel::handleEvent(Timestamp receiveTime) {
    eventHandling_ = true;
    std::shared_ptr<void> guard;
    if (tied_) {
        guard = tie_.lock();  // 拿到强引用，回调期间对象不会被销毁
    }
    if (guard || !tied_) {
        handleEventWithGuard(receiveTime);
    }
    eventHandling_ = false;
}

void Channel::handleEventWithGuard(Timestamp receiveTime) {
    // EPOLLHUP：对端完全关闭。若同时可读，说明还有残留数据，优先交给读回调处理
    if ((revents_ & EPOLLHUP) && !(revents_ & EPOLLIN)) {
        if (closeCallback_) closeCallback_();
        return;
    }
    if (revents_ & EPOLLERR) {
        if (errorCallback_) errorCallback_();
        return;
    }
    // EPOLLRDHUP：对端 shutdown(SHUT_WR)，此时 read 会返回 0
    if (revents_ & (EPOLLIN | EPOLLPRI | EPOLLRDHUP)) {
        if (readCallback_) readCallback_(receiveTime);
    }
    if (revents_ & EPOLLOUT) {
        if (writeCallback_) writeCallback_();
    }
}

void Channel::update() {
    addedToLoop_ = true;
    loop_->updateChannel(this);
}

void Channel::remove() {
    addedToLoop_ = false;
    loop_->removeChannel(this);
}

}  // namespace net
