// Timer.h -- 单个定时器
#ifndef MINET_TIMER_H
#define MINET_TIMER_H

#include <atomic>
#include <cstdint>
#include <utility>

#include "net/Callbacks.h"
#include "net/Timestamp.h"

namespace net {

class Timer {
public:
    Timer(TimerCallback cb, Timestamp when, double interval)
        : callback_(std::move(cb)),
          expiration_(when),
          interval_(interval),
          repeat_(interval > 0.0),
          sequence_(++s_numCreated_) {}

    void run() const {
        if (callback_) callback_();
    }

    Timestamp expiration() const { return expiration_; }
    bool repeat() const { return repeat_; }
    int64_t sequence() const { return sequence_; }

    // 周期定时器触发后重新计算到期时间。
    // 注意是基于 now 而不是基于原到期时间，避免处理耗时导致"追帧"雪崩。
    void restart(Timestamp now) {
        if (repeat_) {
            expiration_ = addTime(now, interval_);
        } else {
            expiration_ = Timestamp::invalid();
        }
    }

private:
    const TimerCallback callback_;
    Timestamp expiration_;
    const double interval_;
    const bool repeat_;
    const int64_t sequence_;

    static std::atomic<int64_t> s_numCreated_;
};

}  // namespace net

#endif  // MINET_TIMER_H
