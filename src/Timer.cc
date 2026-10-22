#include "net/Timer.h"

namespace net {

// 静态成员定义：每个 Timer 构造时自增，配合 TimerId 识别地址复用
std::atomic<int64_t> Timer::s_numCreated_(0);

}  // namespace net
