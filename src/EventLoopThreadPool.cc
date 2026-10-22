#include "net/EventLoopThreadPool.h"

#include <string>

#include "net/EventLoop.h"
#include "net/EventLoopThread.h"

namespace net {

EventLoopThreadPool::EventLoopThreadPool(EventLoop* baseLoop, const std::string& nameArg)
    : baseLoop_(baseLoop),
      name_(nameArg),
      started_(false),
      numThreads_(0),
      next_(0) {}

EventLoopThreadPool::~EventLoopThreadPool() = default;

void EventLoopThreadPool::start(const ThreadInitCallback& cb) {
    started_ = true;
    loops_.reserve(static_cast<size_t>(numThreads_));
    for (int i = 0; i < numThreads_; ++i) {
        // 用 std::string 拼接：变长数组（VLA）不是标准 C++，g++ 只是扩展支持
        std::string threadName = name_ + std::to_string(i);
        threads_.emplace_back(new EventLoopThread(cb, threadName));
        loops_.push_back(threads_.back()->startLoop());
    }
    // 线程数为 0 时，回调在 baseLoop 上执行一次，保持语义一致
    if (numThreads_ == 0 && cb) {
        cb(baseLoop_);
    }
}

EventLoop* EventLoopThreadPool::getNextLoop() {
    EventLoop* loop = baseLoop_;
    if (!loops_.empty()) {
        loop = loops_[static_cast<size_t>(next_)];
        ++next_;
        if (static_cast<size_t>(next_) >= loops_.size()) {
            next_ = 0;
        }
    }
    return loop;
}

std::vector<EventLoop*> EventLoopThreadPool::getAllLoops() {
    if (loops_.empty()) {
        return std::vector<EventLoop*>(1, baseLoop_);
    }
    return loops_;
}

}  // namespace net
