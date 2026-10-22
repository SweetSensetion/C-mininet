// Timestamp.h -- 时间戳封装（微秒精度）
//
// 为什么不直接用 time_t / struct timeval：
//   1. time_t 只有秒级精度，做不了 P99 延迟统计；
//   2. timeval 需要两个字段，比较、相减都要手写，容易出错；
//   3. 用一个 int64 表示"距 Unix 纪元的微秒数"，比较就是整数比较，
//      排序、做 map key、算差值都非常方便（本项目定时器就用它做 key）。
#ifndef MINET_TIMESTAMP_H
#define MINET_TIMESTAMP_H

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <sys/time.h>
#include <utility>

namespace net {

class Timestamp {
public:
    static const int kMicroSecondsPerSecond = 1000 * 1000;

    Timestamp() : microSecondsSinceEpoch_(0) {}

    explicit Timestamp(int64_t microSecondsSinceEpochArg)
        : microSecondsSinceEpoch_(microSecondsSinceEpochArg) {}

    void swap(Timestamp& that) {
        std::swap(microSecondsSinceEpoch_, that.microSecondsSinceEpoch_);
    }

    std::string toString() const {
        char buf[64] = {0};
        time_t seconds = static_cast<time_t>(microSecondsSinceEpoch_ / kMicroSecondsPerSecond);
        int microseconds = static_cast<int>(microSecondsSinceEpoch_ % kMicroSecondsPerSecond);
        struct tm tm_time;
        localtime_r(&seconds, &tm_time);
        snprintf(buf, sizeof(buf), "%04d%02d%02d %02d:%02d:%02d.%06d",
                 tm_time.tm_year + 1900, tm_time.tm_mon + 1, tm_time.tm_mday,
                 tm_time.tm_hour, tm_time.tm_min, tm_time.tm_sec, microseconds);
        return buf;
    }

    bool valid() const { return microSecondsSinceEpoch_ > 0; }

    int64_t microSecondsSinceEpoch() const { return microSecondsSinceEpoch_; }

    // 供日志/统计使用的秒 + 微秒拆分
    time_t seconds() const { return static_cast<time_t>(microSecondsSinceEpoch_ / kMicroSecondsPerSecond); }

    static Timestamp now() {
        struct timeval tv;
        gettimeofday(&tv, nullptr);
        return Timestamp(static_cast<int64_t>(tv.tv_sec) * kMicroSecondsPerSecond + tv.tv_usec);
    }

    static Timestamp invalid() { return Timestamp(); }

    static Timestamp fromUnixTime(time_t t) { return fromUnixTime(t, 0); }

    static Timestamp fromUnixTime(time_t t, int microseconds) {
        return Timestamp(static_cast<int64_t>(t) * kMicroSecondsPerSecond + microseconds);
    }

private:
    int64_t microSecondsSinceEpoch_;
};

inline bool operator<(Timestamp lhs, Timestamp rhs) {
    return lhs.microSecondsSinceEpoch() < rhs.microSecondsSinceEpoch();
}

inline bool operator==(Timestamp lhs, Timestamp rhs) {
    return lhs.microSecondsSinceEpoch() == rhs.microSecondsSinceEpoch();
}

inline bool operator<=(Timestamp lhs, Timestamp rhs) {
    return lhs.microSecondsSinceEpoch() <= rhs.microSecondsSinceEpoch();
}

// 返回秒数（浮点），用于统计耗时
inline double timeDifference(Timestamp high, Timestamp low) {
    int64_t diff = high.microSecondsSinceEpoch() - low.microSecondsSinceEpoch();
    return static_cast<double>(diff) / Timestamp::kMicroSecondsPerSecond;
}

inline Timestamp addTime(Timestamp timestamp, double seconds) {
    int64_t delta = static_cast<int64_t>(seconds * Timestamp::kMicroSecondsPerSecond);
    return Timestamp(timestamp.microSecondsSinceEpoch() + delta);
}

}  // namespace net

#endif  // MINET_TIMESTAMP_H
