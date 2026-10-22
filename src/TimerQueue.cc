#include "net/TimerQueue.h"

#include <algorithm>
#include <iterator>

#include "net/EventLoop.h"
#include "net/Logging.h"

namespace net {

namespace {
const int kDefaultWaitMs = 10000;   // 无定时器时最多睡 10 秒（可被 wakeup 打断）
const int kMaxWaitMs = 10000;       // epoll_wait 超时上限，保证 quit 能及时生效
}

TimerQueue::TimerQueue(EventLoop* loop)
    : loop_(loop), callingExpiredTimers_(false) {}

TimerQueue::~TimerQueue() = default;

TimerId TimerQueue::addTimer(TimerCallback cb, Timestamp when, double interval) {
    // 用 shared_ptr 持有，lambda 拷贝安全，跨线程投递时也不会悬空
    std::shared_ptr<Timer> timer(new Timer(std::move(cb), when, interval));
    TimerId id(timer.get(), timer->sequence());

    // 必须回到 loop 线程操作 timers_：std::set 不是线程安全的
    loop_->runInLoop([this, timer]() { addTimerInLoop(timer); });
    return id;
}

void TimerQueue::addTimerInLoop(const std::shared_ptr<Timer>& timer) {
    loop_->assertInLoopThread();
    bool earliestChanged = insert(timer);
    if (earliestChanged) {
        // 关键：新定时器比当前睡眠超时更早，必须立刻唤醒 epoll_wait，
        // 否则会按旧的超时时间睡过头，定时器被延迟执行。
        loop_->wakeup();
    }
}

bool TimerQueue::insert(const std::shared_ptr<Timer>& timer) {
    bool earliestChanged = timers_.empty() || timer->expiration() < timers_.begin()->first;
    timers_.insert(Entry(timer->expiration(), timer));
    return earliestChanged;
}

void TimerQueue::cancel(TimerId timerId) {
    loop_->runInLoop([this, timerId]() { cancelInLoop(timerId); });
}

void TimerQueue::cancelInLoop(TimerId timerId) {
    loop_->assertInLoopThread();
    // 线性查找：定时器数量在万级以下，O(n) 可接受；
    // 若要 O(1)，需额外维护 Timer* -> iterator 的哈希表。
    for (auto it = timers_.begin(); it != timers_.end(); ++it) {
        if (it->second.get() == timerId.timer_) {
            timers_.erase(it);
            return;
        }
    }
    // 定时器正在执行回调：此时不能从 timers_ 删除（迭代器/容器状态在回调栈上），
    // 先记下来，等 reset 阶段跳过重启即可。
    if (callingExpiredTimers_) {
        cancelingTimers_.insert(timerId.timer_);
    }
}

std::vector<TimerQueue::Entry> TimerQueue::getExpired(Timestamp now) {
    std::vector<Entry> expired;
    auto end = timers_.begin();
    while (end != timers_.end() && end->first <= now) {
        ++end;
    }
    std::copy(timers_.begin(), end, std::back_inserter(expired));
    timers_.erase(timers_.begin(), end);
    return expired;
}

void TimerQueue::handleExpired() {
    loop_->assertInLoopThread();
    Timestamp now(Timestamp::now());
    std::vector<Entry> expired = getExpired(now);

    callingExpiredTimers_ = true;
    for (const Entry& it : expired) {
        it.second->run();
    }
    callingExpiredTimers_ = false;

    reset(expired, now);
}

void TimerQueue::reset(const std::vector<Entry>& expired, Timestamp now) {
    for (const Entry& it : expired) {
        // 周期定时器重启；已被 cancel 的跳过
        if (it.second->repeat() &&
            cancelingTimers_.find(it.second.get()) == cancelingTimers_.end()) {
            it.second->restart(now);
            insert(it.second);
        }
    }
    cancelingTimers_.clear();
}

int TimerQueue::nextTimeoutMs() const {
    if (timers_.empty()) {
        return kDefaultWaitMs;
    }
    Timestamp now = Timestamp::now();
    Timestamp when = timers_.begin()->first;
    if (when <= now) {
        return 0;  // 已到期，立即返回处理
    }
    int64_t micros = when.microSecondsSinceEpoch() - now.microSecondsSinceEpoch();
    int64_t ms = micros / 1000;
    // 向上取整：不足 1ms 也要等 1ms，否则会空转
    if (ms * 1000 < micros) {
        ++ms;
    }
    if (ms > kMaxWaitMs) {
        ms = kMaxWaitMs;
    }
    return static_cast<int>(ms);
}

}  // namespace net
