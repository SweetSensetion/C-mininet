#include "net/ThreadPool.h"

#include <exception>

#include "net/Logging.h"

namespace net {

ThreadPool::ThreadPool(size_t threadCount, size_t maxQueueSize)
    : threadCount_(threadCount == 0 ? 1 : threadCount),
      maxQueueSize_(maxQueueSize == 0 ? 1 : maxQueueSize),
      running_(false),
      started_(false) {
    threads_.reserve(threadCount_);
}

ThreadPool::~ThreadPool() {
    if (started_) {
        stop();
    }
}

void ThreadPool::start() {
    if (started_) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = true;
        started_ = true;
    }
    for (size_t i = 0; i < threadCount_; ++i) {
        threads_.emplace_back(&ThreadPool::workerLoop, this);
    }
}

void ThreadPool::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
    }
    // 必须 notify_all：notify_one 只会叫醒一个 worker，其余会永远睡在 wait 上
    notEmpty_.notify_all();
    notFull_.notify_all();

    for (std::thread& t : threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
    threads_.clear();
    started_ = false;
}

bool ThreadPool::run(Task task) {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        // 带谓词的 wait：防虚假唤醒；返回 false 表示可能永远等不到空位
        notFull_.wait(lock, [this] {
            return !running_ || tasks_.size() < maxQueueSize_;
        });
        if (!running_) {
            return false;
        }
        tasks_.push_back(std::move(task));
    }
    notEmpty_.notify_one();
    return true;
}

size_t ThreadPool::queueSize() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tasks_.size();
}

void ThreadPool::workerLoop() {
    for (;;) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            notEmpty_.wait(lock, [this] { return !running_ || !tasks_.empty(); });
            // 优雅退出：先把已入队的任务执行完，再退出，
            // 避免"请求已受理但从未处理"的静默丢失
            if (!running_ && tasks_.empty()) {
                return;
            }
            task = std::move(tasks_.front());
            tasks_.pop_front();
        }
        notFull_.notify_one();   // 及时唤醒可能在等空位的生产者

        // 任何异常都不能逃出线程函数，否则 std::terminate 直接杀掉进程
        try {
            task();
        } catch (const std::exception& e) {
            LOG_ERROR << "thread pool task threw: " << e.what();
        } catch (...) {
            LOG_ERROR << "thread pool task threw unknown exception";
        }
    }
}

}  // namespace net
