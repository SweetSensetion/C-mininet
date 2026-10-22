// EventLoopThreadPool.h -- subReactor 线程池
//
// 只负责"创建 N 个 EventLoopThread 并按轮询分配"，不做任务调度。
// 注意它和 ThreadPool 的区别：
//   * EventLoopThreadPool：每个线程有自己的 epoll 实例，处理绑定其上的连接（长生命周期）；
//   * ThreadPool：所有线程共享一个任务队列，处理一次性计算任务（短生命周期）。
// 两者解决的是不同问题，不能互相替代。
#ifndef MINET_EVENTLOOPTHREADPOOL_H
#define MINET_EVENTLOOPTHREADPOOL_H

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace net {

class EventLoop;
class EventLoopThread;

class EventLoopThreadPool {
public:
    using ThreadInitCallback = std::function<void(EventLoop*)>;

    EventLoopThreadPool(EventLoop* baseLoop, const std::string& nameArg);
    ~EventLoopThreadPool();

    EventLoopThreadPool(const EventLoopThreadPool&) = delete;
    EventLoopThreadPool& operator=(const EventLoopThreadPool&) = delete;

    void setThreadNum(int numThreads) { numThreads_ = numThreads; }
    void start(const ThreadInitCallback& cb = ThreadInitCallback());

    // 轮询取下一个 loop；线程数为 0 时退化为单 Reactor（返回 baseLoop）
    EventLoop* getNextLoop();
    std::vector<EventLoop*> getAllLoops();

    bool started() const { return started_; }
    const std::string& name() const { return name_; }

private:
    EventLoop* baseLoop_;
    std::string name_;
    bool started_;
    int numThreads_;
    int next_;   // 只在 baseLoop 线程被访问，因此不需要原子
    std::vector<std::unique_ptr<EventLoopThread>> threads_;
    std::vector<EventLoop*> loops_;
};

}  // namespace net

#endif  // MINET_EVENTLOOPTHREADPOOL_H
