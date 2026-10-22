// EPollPoller.h -- epoll 封装
#ifndef MINET_EPOLLPOLLER_H
#define MINET_EPOLLPOLLER_H

#include <sys/epoll.h>

#include <vector>

#include "net/Poller.h"

namespace net {

class EPollPoller : public Poller {
public:
    explicit EPollPoller(EventLoop* loop);
    ~EPollPoller() override;

    Timestamp poll(int timeoutMs, ChannelList* activeChannels) override;
    void updateChannel(Channel* channel) override;
    void removeChannel(Channel* channel) override;

private:
    static const int kInitEventListSize = 16;

    void fillActiveChannels(int numEvents, ChannelList* activeChannels) const;
    void update(int operation, Channel* channel);

    int epollfd_;
    // events_ 随并发连接数增长而扩容，避免固定 1024 上限
    std::vector<struct epoll_event> events_;
};

}  // namespace net

#endif  // MINET_EPOLLPOLLER_H
