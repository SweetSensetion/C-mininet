#include "net/Logging.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <sys/time.h>
#include <unistd.h>

namespace net {

namespace {

std::mutex g_logMutex;
Logger::LogLevel g_logLevel = Logger::INFO;

const char* levelName(Logger::LogLevel level) {
    switch (level) {
        case Logger::TRACE: return "TRACE";
        case Logger::DEBUG: return "DEBUG";
        case Logger::INFO:  return "INFO ";
        case Logger::WARN:  return "WARN ";
        case Logger::ERROR: return "ERROR";
        case Logger::FATAL: return "FATAL";
        default:            return "UNKWN";
    }
}

// 写满 len 字节，失败则放弃（日志不能反过来把进程拖垮）。
// 返回值的“使用”同时消除了 -Wunused-result 告警。
void appendAll(int fd, const char* data, size_t len) {
    size_t written = 0;
    while (written < len) {
        ssize_t n = ::write(fd, data + written, len - written);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            return;
        }
        written += static_cast<size_t>(n);
    }
}

}  // namespace

Logger::Logger(const char* file, int line, LogLevel level)
    : level_(level), file_(file), line_(line), enabled_(level >= g_logLevel) {}

Logger::~Logger() {
    if (!enabled_) return;  // 级别过滤：连格式化都不做

    // 一次日志只锁一次，保证多线程下不会出现半行交错
    std::lock_guard<std::mutex> lock(g_logMutex);

    // 只取文件名，去掉路径前缀
    const char* base = strrchr(file_, '/');
    base = base ? base + 1 : file_;

    char header[64];
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    struct tm tm_time;
    localtime_r(&tv.tv_sec, &tm_time);
    int n = snprintf(header, sizeof(header), "%02d:%02d:%02d.%06ld ",
                     tm_time.tm_hour, tm_time.tm_min, tm_time.tm_sec,
                     static_cast<long>(tv.tv_usec));

    std::string payload = stream_.str();
    // 用 write 而不是 fprintf：避免 stdio 缓冲区在 fork 后重复输出。
    // 返回值必须处理：GCC 的 warn_unused_result 会告警，而且写日志失败时
    // 我们也不该假装成功——stderr 都写不进去时只能放弃。
    appendAll(STDERR_FILENO, header, static_cast<size_t>(n));
    appendAll(STDERR_FILENO, levelName(level_), 5);
    appendAll(STDERR_FILENO, " ", 1);
    appendAll(STDERR_FILENO, base, strlen(base));
    char linebuf[16];
    int m = snprintf(linebuf, sizeof(linebuf), ":%d ", line_);
    appendAll(STDERR_FILENO, linebuf, static_cast<size_t>(m));
    appendAll(STDERR_FILENO, payload.data(), payload.size());
    appendAll(STDERR_FILENO, "\n", 1);

    if (level_ == FATAL) {
        abort();
    }
}

void Logger::setLogLevel(LogLevel level) { g_logLevel = level; }

Logger::LogLevel Logger::logLevel() { return g_logLevel; }

}  // namespace net
