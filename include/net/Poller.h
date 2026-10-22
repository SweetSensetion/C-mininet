// Poller.h -- IO 复用器抽象
//
// 抽成基类是为了让 EventLoop 不依赖 epoll 细节：换 kqueue 只需新增一个
// 子类，EventLoop 一行不改。这是"依赖倒置"在网络库里的实际收益。
#ifndef MINET_POLLER_H
#define MINET_POLLER_H

#include <map>
#include <vector>

#include "net/Timestamp.h"

namespace net {

class Channel;
class EventLoop;

class Poller {
public:
    using ChannelList = std::vector<Channel*>;

    explicit Poller(EventLoop* loop) : ownerLoop_(loop) {}
    virtual ~Poller() = default;

    Poller(const Poller&) = delete;
    Poller& operator=(const Poller&) = delete;

    // 返回 poll 时刻的时间戳，避免每个事件都调一次 gettimeofday
    virtual Timestamp poll(int timeoutMs, ChannelList* activeChannels) = 0;

    virtual void updateChannel(Channel* channel) = 0;
    virtual void removeChannel(Channel* channel) = 0;

    bool hasChannel(Channel* channel) const;

protected:
    using ChannelMap = std::map<int, Channel*>;
    ChannelMap channels_;

private:
    EventLoop* ownerLoop_;
};

}  // namespace net

#endif  // MINET_POLLER_H
