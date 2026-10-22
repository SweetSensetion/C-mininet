// TimerQueue.h -- 定时器集合
//
// 实现：std::set<std::pair<Timestamp, shared_ptr<Timer>>>，按到期时间有序。
//   * 插入 / 删除 O(log n)
//   * 取最近到期时间 O(1)
//   * 到期扫描只遍历到期的前缀，不碰其余元素
//
// 为什么不用 std::priority_queue：它不支持删除任意元素，而 cancel 是必需能力。
// 为什么不用时间轮：时间轮适合"海量定时器 + 精度要求不高"（如 Netty 的连接
// 超时），本项目定时器数量与连接数同阶（万级），红黑树的 log n 完全够用，
// 且实现简单、精度无损。
#ifndef MINET_TIMERQUEUE_H
#define MINET_TIMERQUEUE_H

#include <set>
#include <utility>
#include <vector>

#include "net/Callbacks.h"
#include "net/Timer.h"
#include "net/TimerId.h"
#include "net/Timestamp.h"

namespace net {

class EventLoop;

class TimerQueue {
public:
    explicit TimerQueue(EventLoop* loop);
    ~TimerQueue();

    TimerQueue(const TimerQueue&) = delete;
    TimerQueue& operator=(const TimerQueue&) = delete;

    TimerId addTimer(TimerCallback cb, Timestamp when, double interval);
    void cancel(TimerId timerId);

    // 距离下一个定时器到期的毫秒数；无定时器时返回一个较大值
    int nextTimeoutMs() const;

    // 由 EventLoop 在每轮 poll 之后调用
    void handleExpired();

private:
    using Entry = std::pair<Timestamp, std::shared_ptr<Timer>>;
    using TimerList = std::set<Entry>;

    void addTimerInLoop(const std::shared_ptr<Timer>& timer);
    bool insert(const std::shared_ptr<Timer>& timer);
    void cancelInLoop(TimerId timerId);
    std::vector<Entry> getExpired(Timestamp now);
    void reset(const std::vector<Entry>& expired, Timestamp now);

    EventLoop* loop_;
    TimerList timers_;
    std::set<Timer*> cancelingTimers_;
    bool callingExpiredTimers_;
};

}  // namespace net

#endif  // MINET_TIMERQUEUE_H
