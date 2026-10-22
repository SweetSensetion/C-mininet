// Channel.h -- fd 与其事件回调的绑定
//
// Channel 是 Reactor 的"事件分发单元"：它不拥有 fd（fd 由 Socket 拥有），
// 只保存 fd、关心的事件集合、以及四个回调。Poller 返回就绪事件后，
// EventLoop 通过 Channel 把"fd 可读"翻译成"调用 readCallback_"。
//
// 为什么要多这一层抽象而不是直接在 EventLoop 里 switch(fd)：
//   1. fd 上的事件语义（读/写/关闭/错误）与业务处理解耦；
//   2. 每个 fd 的注册状态（index_）集中管理，避免重复 ADD/DEL；
//   3. 支持 tie()，解决"回调执行中对象被销毁"的 use-after-free。
#ifndef MINET_CHANNEL_H
#define MINET_CHANNEL_H

#include <sys/epoll.h>

#include <functional>
#include <memory>
#include <utility>

#include "net/Timestamp.h"

namespace net {

class EventLoop;

class Channel {
public:
    using EventCallback = std::function<void()>;
    using ReadEventCallback = std::function<void(Timestamp)>;

    Channel(EventLoop* loop, int fd);
    ~Channel();

    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    void handleEvent(Timestamp receiveTime);

    void setReadCallback(ReadEventCallback cb) { readCallback_ = std::move(cb); }
    void setWriteCallback(EventCallback cb) { writeCallback_ = std::move(cb); }
    void setCloseCallback(EventCallback cb) { closeCallback_ = std::move(cb); }
    void setErrorCallback(EventCallback cb) { errorCallback_ = std::move(cb); }

    // 绑定所有者（通常是 TcpConnection 的 shared_ptr）。
    // handleEvent 期间 lock 住 weak_ptr，保证回调中 delete 连接也不会
    // 让 this 悬空——这是 Reactor 里最容易被忽略的一处生命周期保护。
    void tie(const std::shared_ptr<void>& obj) {
        tie_ = obj;
        tied_ = true;
    }

    int fd() const { return fd_; }
    int events() const { return events_; }
    void set_revents(int revt) { revents_ = revt; }
    bool isNoneEvent() const { return events_ == kNoneEvent; }

    bool isWriting() const { return (events_ & kWriteEvent) != 0; }
    bool isReading() const { return (events_ & kReadEvent) != 0; }

    void enableReading() {
        events_ |= kReadEvent;
        update();
    }
    void disableReading() {
        events_ &= ~kReadEvent;
        update();
    }
    void enableWriting() {
        events_ |= kWriteEvent;
        update();
    }
    void disableWriting() {
        events_ &= ~kWriteEvent;
        update();
    }
    void disableAll() {
        events_ = kNoneEvent;
        update();
    }

    // index_ 的三态：-1 未注册、1 已注册、2 已删除。
    // 有了它，updateChannel 才知道该用 ADD 还是 MOD。
    int index() const { return index_; }
    void set_index(int idx) { index_ = idx; }

    EventLoop* ownerLoop() { return loop_; }
    void remove();

private:
    void update();
    void handleEventWithGuard(Timestamp receiveTime);

    static const int kNoneEvent;
    static const int kReadEvent;
    static const int kWriteEvent;

    EventLoop* loop_;
    const int fd_;
    int events_;
    int revents_;
    int index_;
    bool addedToLoop_;
    bool eventHandling_;
    bool tied_;
    std::weak_ptr<void> tie_;

    ReadEventCallback readCallback_;
    EventCallback writeCallback_;
    EventCallback closeCallback_;
    EventCallback errorCallback_;
};

}  // namespace net

#endif  // MINET_CHANNEL_H
