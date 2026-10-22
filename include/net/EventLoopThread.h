// EventLoopThread.h -- "启动一个线程并跑一个 EventLoop"
//
// 这是 one loop per thread 的落地方式：每个 subReactor 都是一个
// EventLoopThread，内部 EventLoop 的生命周期完全属于该线程。
#ifndef MINET_EVENTLOOPTHREAD_H
#define MINET_EVENTLOOPTHREAD_H

#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace net {

class EventLoop;

class EventLoopThread {
public:
    using ThreadInitCallback = std::function<void(EventLoop*)>;

    explicit EventLoopThread(const ThreadInitCallback& cb = ThreadInitCallback(),
                             const std::string& name = std::string());
    ~EventLoopThread();

    EventLoopThread(const EventLoopThread&) = delete;
    EventLoopThread& operator=(const EventLoopThread&) = delete;

    // 启动线程并阻塞到 EventLoop 构造完成，返回的指针在 loop 退出前有效
    EventLoop* startLoop();

private:
    void threadFunc();

    EventLoop* loop_;
    bool exiting_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cond_;
    ThreadInitCallback callback_;
    std::string name_;
};

}  // namespace net

#endif  // MINET_EVENTLOOPTHREAD_H
