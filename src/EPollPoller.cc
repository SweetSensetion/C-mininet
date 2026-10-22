#include "net/EPollPoller.h"

#include <cstring>
#include <unistd.h>

#include "net/Channel.h"
#include "net/EventLoop.h"
#include "net/Logging.h"

namespace net {

namespace {

const int kNew = -1;      // 尚未加入 epoll
const int kAdded = 1;     // 已加入
const int kDeleted = 2;   // 已从 epoll 摘除

}  // namespace

EPollPoller::EPollPoller(EventLoop* loop)
    : Poller(loop),
      epollfd_(::epoll_create1(EPOLL_CLOEXEC)),
      events_(kInitEventListSize) {
    if (epollfd_ < 0) {
        LOG_FATAL << "epoll_create1 failed, errno=" << errno;
    }
}

EPollPoller::~EPollPoller() {
    ::close(epollfd_);
}

Timestamp EPollPoller::poll(int timeoutMs, ChannelList* activeChannels) {
    int numEvents = ::epoll_wait(epollfd_, events_.data(),
                                 static_cast<int>(events_.size()), timeoutMs);
    int savedErrno = errno;
    Timestamp now(Timestamp::now());

    if (numEvents > 0) {
        fillActiveChannels(numEvents, activeChannels);
        // 事件数打满说明可能还有更多事件没取出来，扩容降低下次系统调用次数
        if (static_cast<size_t>(numEvents) == events_.size()) {
            events_.resize(events_.size() * 2);
        }
    } else if (numEvents == 0) {
        // 超时：正常现象，交给 EventLoop 处理到期定时器
    } else {
        // EINTR 必须当作"本轮无事件"处理，不能当错误退出循环
        if (savedErrno != EINTR) {
            errno = savedErrno;
            LOG_ERROR << "epoll_wait failed, errno=" << savedErrno;
        }
    }
    return now;
}

void EPollPoller::fillActiveChannels(int numEvents, ChannelList* activeChannels) const {
    for (int i = 0; i < numEvents; ++i) {
        // data.ptr 是注册时塞进去的 Channel 指针，省掉 fd -> Channel 的查表
        Channel* channel = static_cast<Channel*>(events_[i].data.ptr);
        channel->set_revents(events_[i].events);
        activeChannels->push_back(channel);
    }
}

void EPollPoller::updateChannel(Channel* channel) {
    const int index = channel->index();
    const int fd = channel->fd();

    if (index == kNew || index == kDeleted) {
        if (index == kNew) {
            channels_[fd] = channel;
        }
        channel->set_index(kAdded);
        update(EPOLL_CTL_ADD, channel);
    } else {
        // 已注册：关注的事件集合为空时要 DEL，否则内核仍会为该 fd 排队事件
        if (channel->isNoneEvent()) {
            update(EPOLL_CTL_DEL, channel);
            channel->set_index(kDeleted);
        } else {
            update(EPOLL_CTL_MOD, channel);
        }
    }
}

void EPollPoller::removeChannel(Channel* channel) {
    const int fd = channel->fd();
    channels_.erase(fd);

    if (channel->index() == kAdded) {
        update(EPOLL_CTL_DEL, channel);
    }
    channel->set_index(kNew);
}

void EPollPoller::update(int operation, Channel* channel) {
    struct epoll_event event;
    std::memset(&event, 0, sizeof event);
    event.events = channel->events();
    event.data.ptr = channel;

    if (::epoll_ctl(epollfd_, operation, channel->fd(), &event) < 0) {
        // 删除一个已经关闭的 fd 会得到 EBADF/ENOENT，属于正常竞态，忽略即可
        if (operation == EPOLL_CTL_DEL &&
            (errno == EBADF || errno == ENOENT || errno == EPERM)) {
            return;
        }
        LOG_ERROR << "epoll_ctl op=" << operation << " fd=" << channel->fd()
                  << " failed, errno=" << errno;
    }
}

}  // namespace net
