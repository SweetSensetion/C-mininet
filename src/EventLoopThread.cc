#include "net/EventLoopThread.h"

#include "net/EventLoop.h"

namespace net {

EventLoopThread::EventLoopThread(const ThreadInitCallback& cb, const std::string& name)
    : loop_(nullptr), exiting_(false), callback_(cb), name_(name) {}

EventLoopThread::~EventLoopThread() {
    exiting_ = true;
    EventLoop* loop = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        loop = loop_;   // 线程已退出时 loop_ 会被置空，此时无需 quit
    }
    if (loop != nullptr) {
        // 让 loop 退出阻塞的 epoll_wait，再 join，保证线程不会在对象析构后继续跑
        loop->quit();
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

EventLoop* EventLoopThread::startLoop() {
    thread_ = std::thread(&EventLoopThread::threadFunc, this);

    EventLoop* loop = nullptr;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        // 必须等 EventLoop 构造完成再返回：调用方拿到指针后可能立刻 updateChannel，
        // 此时 loop 若还没建好 poller，就会崩在空指针上
        cond_.wait(lock, [this] { return loop_ != nullptr; });
        loop = loop_;
    }
    return loop;
}

void EventLoopThread::threadFunc() {
    // EventLoop 建在栈上：它的生命周期严格等于本线程的函数栈，
    // 天然满足"loop 只能由所属线程创建和销毁"
    EventLoop loop;

    if (callback_) {
        callback_(&loop);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        loop_ = &loop;
        cond_.notify_one();
    }

    loop.loop();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        loop_ = nullptr;
    }
}

}  // namespace net
