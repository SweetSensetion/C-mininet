// TimerId.h -- 定时器句柄（值语义，可拷贝）
//
// 为什么不直接返回 Timer*：
//   * 裸指针无法区分"已取消"和"已触发销毁"，用户 cancel 时可能踩到野指针；
//   * TimerId 用 (指针, 序号) 二元组，序号随每次创建递增，即使地址被复用
//     也能识别出不是同一个定时器，避免 ABA。
#ifndef MINET_TIMERID_H
#define MINET_TIMERID_H

#include <cstdint>

namespace net {

class Timer;

class TimerId {
public:
    TimerId() : timer_(nullptr), sequence_(0) {}
    TimerId(Timer* timer, int64_t seq) : timer_(timer), sequence_(seq) {}

    friend class TimerQueue;

private:
    Timer* timer_;
    int64_t sequence_;
};

}  // namespace net

#endif  // MINET_TIMERID_H
