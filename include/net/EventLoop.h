// EventLoop.h -- Reactor 的心脏
//
// 一个 EventLoop 绑定一个线程，生命周期内不迁移。
// 对外提供三类能力：
//   1. loop()：驱动事件循环（只能由所属线程调用）；
//   2. runInLoop / queueInLoop：把任务安全地投递到本线程执行；
//   3. runAt / runAfter / runEvery：定时任务。
#ifndef MINET_EVENTLOOP_H
#define MINET_EVENTLOOP_H

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "net/Callbacks.h"
#include "net/TimerId.h"
#include "net/Timestamp.h"

namespace net {

class Channel;
class Poller;
class TimerQueue;

class EventLoop {
public:
    using Functor = std::function<void()>;

    EventLoop();
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    void loop();
    void quit();

    // 同线程直接执行；跨线程入队并唤醒。调用方不需要知道自己身处哪个线程。
    void runInLoop(Functor cb);
    // 总是入队（即使同线程），用于"必须延后执行"的场景（如延迟销毁对象）
    void queueInLoop(Functor cb);

    size_t queueSize() const;

    // 唤醒阻塞中的 epoll_wait（写 eventfd）
    void wakeup();

    void updateChannel(Channel* channel);
    void removeChannel(Channel* channel);
    bool hasChannel(Channel* channel);

    void assertInLoopThread() const;
    bool isInLoopThread() const { return threadId_ == std::this_thread::get_id(); }
    bool eventHandling() const { return eventHandling_; }

    TimerId runAt(Timestamp time, TimerCallback cb);
    TimerId runAfter(double delay, TimerCallback cb);
    TimerId runEvery(double interval, TimerCallback cb);
    void cancel(TimerId timerId);

private:
    void handleWakeup();
    void doPendingFunctors();

    using ChannelList = std::vector<Channel*>;

    std::atomic<bool> looping_;
    std::atomic<bool> quit_;
    bool eventHandling_;
    bool callingPendingFunctors_;
    const std::thread::id threadId_;

    std::unique_ptr<Poller> poller_;
    std::unique_ptr<TimerQueue> timerQueue_;

    int wakeupFd_;                      // eventfd，用于跨线程唤醒
    std::unique_ptr<Channel> wakeupChannel_;

    ChannelList activeChannels_;
    Channel* currentActiveChannel_;

    mutable std::mutex mutex_;
    std::vector<Functor> pendingFunctors_;   // 由 mutex_ 保护
};

}  // namespace net

#endif  // MINET_EVENTLOOP_H
