#include "net/EventLoop.h"

#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <exception>

#include "net/Channel.h"
#include "net/EPollPoller.h"
#include "net/Logging.h"
#include "net/TimerQueue.h"

namespace net {

EventLoop::EventLoop()
    : looping_(false),
      quit_(false),
      eventHandling_(false),
      callingPendingFunctors_(false),
      threadId_(std::this_thread::get_id()),
      poller_(new EPollPoller(this)),
      timerQueue_(new TimerQueue(this)),
      wakeupFd_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)),
      wakeupChannel_(new Channel(this, wakeupFd_)),
      currentActiveChannel_(nullptr) {
    if (wakeupFd_ < 0) {
        LOG_FATAL << "eventfd failed, errno=" << errno;
    }
    wakeupChannel_->setReadCallback([this](Timestamp) { handleWakeup(); });
    wakeupChannel_->enableReading();
}

EventLoop::~EventLoop() {
    // 走到这里时事件循环必须已经退出，否则 poller_ 析构会带走还在监听的 fd
    if (looping_) {
        LOG_ERROR << "EventLoop destroyed while still looping";
    }
    wakeupChannel_->disableAll();
    wakeupChannel_->remove();
    ::close(wakeupFd_);
}

void EventLoop::loop() {
    assertInLoopThread();
    looping_ = true;
    // 注意：这里不能再把 quit_ 置 false，否则在 loop() 之前调用的 quit()
    // 会被吞掉（例如启动阶段收到退出信号）。

    while (!quit_) {
        activeChannels_.clear();

        // 1) 阻塞等待事件；超时时间由最近的定时器决定，保证 quit 也能及时生效
        Timestamp pollReturnTime = poller_->poll(timerQueue_->nextTimeoutMs(), &activeChannels_);

        // 2) 分发 IO 事件
        eventHandling_ = true;
        for (Channel* channel : activeChannels_) {
            currentActiveChannel_ = channel;
            // 业务回调抛异常不能把整个事件循环带走：捕获后记日志，继续服务。
            // 代价是该连接的状态可能已经不一致，所以回调里应自行处理错误。
            try {
                channel->handleEvent(pollReturnTime);
            } catch (const std::exception& e) {
                LOG_ERROR << "exception in channel handler: " << e.what();
            }
        }
        currentActiveChannel_ = nullptr;
        eventHandling_ = false;

        // 3) 处理到期定时器（放在 IO 之后：定时器回调可能关闭连接，
        //    若先执行会让本轮的 activeChannels_ 里出现悬空对象）
        timerQueue_->handleExpired();

        // 4) 执行跨线程投递的任务
        doPendingFunctors();
    }

    looping_ = false;
}

void EventLoop::quit() {
    quit_ = true;
    if (!isInLoopThread()) {
        wakeup();   // 否则对方正睡在 epoll_wait 里，要等到下一个事件/超时才退出
    }
}

void EventLoop::runInLoop(Functor cb) {
    if (isInLoopThread()) {
        cb();                       // 同线程：零开销直接执行
    } else {
        queueInLoop(std::move(cb));
    }
}

void EventLoop::queueInLoop(Functor cb) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pendingFunctors_.push_back(std::move(cb));
    }
    // 两种情况必须唤醒：
    //   1. 跨线程投递——对方正在 epoll_wait 睡觉；
    //   2. 正在执行 pending functor 队列——新任务本轮不会再被取出。
    if (!isInLoopThread() || callingPendingFunctors_) {
        wakeup();
    }
}

size_t EventLoop::queueSize() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pendingFunctors_.size();
}

void EventLoop::wakeup() {
    uint64_t one = 1;
    // eventfd 计数不会溢出（内核用 64 位无符号累加），EAGAIN 只可能出现在
    // 计数达到上限的极端情况，忽略即可
    ssize_t n = ::write(wakeupFd_, &one, sizeof one);
    (void)n;
}

void EventLoop::handleWakeup() {
    uint64_t value;
    ssize_t n = ::read(wakeupFd_, &value, sizeof value);
    (void)n;
}

void EventLoop::doPendingFunctors() {
    std::vector<Functor> functors;
    callingPendingFunctors_ = true;
    {
        // 交换而不是逐个执行：持锁时间 O(1)，避免任务执行期间阻塞其他线程投递
        std::lock_guard<std::mutex> lock(mutex_);
        functors.swap(pendingFunctors_);
    }
    for (const Functor& f : functors) {
        try {
            f();
        } catch (const std::exception& e) {
            LOG_ERROR << "exception in pending functor: " << e.what();
        }
    }
    callingPendingFunctors_ = false;
}

void EventLoop::updateChannel(Channel* channel) {
    assertInLoopThread();
    poller_->updateChannel(channel);
}

void EventLoop::removeChannel(Channel* channel) {
    assertInLoopThread();
    poller_->removeChannel(channel);
}

bool EventLoop::hasChannel(Channel* channel) {
    assertInLoopThread();
    return poller_->hasChannel(channel);
}

void EventLoop::assertInLoopThread() const {
    if (!isInLoopThread()) {
        LOG_FATAL << "EventLoop is running in thread " << threadId_
                  << ", current thread is " << std::this_thread::get_id();
    }
}

TimerId EventLoop::runAt(Timestamp time, TimerCallback cb) {
    return timerQueue_->addTimer(std::move(cb), time, 0.0);
}

TimerId EventLoop::runAfter(double delay, TimerCallback cb) {
    Timestamp time = addTime(Timestamp::now(), delay);
    return runAt(time, std::move(cb));
}

TimerId EventLoop::runEvery(double interval, TimerCallback cb) {
    Timestamp time = addTime(Timestamp::now(), interval);
    return timerQueue_->addTimer(std::move(cb), time, interval);
}

void EventLoop::cancel(TimerId timerId) {
    timerQueue_->cancel(timerId);
}

}  // namespace net
