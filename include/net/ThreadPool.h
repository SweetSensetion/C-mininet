// ThreadPool.h -- 固定大小线程池（生产者-消费者）
//
// 模型：
//   生产者（IO 线程 / main）  ──push──>  [ 有界任务队列 ]  ──pop──>  消费者（worker）
//                                        mutex + 两个条件变量
//
// 为什么用"一把锁 + 两个条件变量"而不是无锁队列：
//   * 任务粒度通常在微秒级以上，未竞争的 mutex 开销约 20ns，可以忽略；
//   * 无锁 MPMC 队列需要 hazard pointer / epoch 回收才能安全释放节点，
//     C++11 没有 std::atomic<std::shared_ptr>，自己实现极易写出 ABA；
//   * 面试时能讲清"为什么这里的锁不是瓶颈"，比堆一个半正确的无锁队列更有价值。
#ifndef MINET_THREADPOOL_H
#define MINET_THREADPOOL_H

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace net {

class ThreadPool {
public:
    using Task = std::function<void()>;

    // maxQueueSize 是背压的抓手：队列满时 run() 返回 false，由调用方降级，
    // 而不是无限堆积直到 OOM。
    explicit ThreadPool(size_t threadCount, size_t maxQueueSize = 100000);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void start();
    void stop();

    // 返回 false 表示线程池已停止或队列已满。
    // 注意：不要在池内任务中调用本函数——若队列已满，worker 会等待空闲槽位，
    // 而槽位只能由 worker 自己消费，形成死锁。
    bool run(Task task);

    size_t queueSize() const;
    size_t threadCount() const { return threadCount_; }

private:
    void workerLoop();

    const size_t threadCount_;
    const size_t maxQueueSize_;

    mutable std::mutex mutex_;
    std::condition_variable notEmpty_;
    std::condition_variable notFull_;
    std::deque<Task> tasks_;

    std::vector<std::thread> threads_;
    bool running_;
    bool started_;
};

}  // namespace net

#endif  // MINET_THREADPOOL_H
