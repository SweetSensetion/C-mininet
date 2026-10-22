// Logging.h -- 极简日志
//
// 设计取舍：
//   * 不做异步日志（单独线程 + 双缓冲），因为日志不是本项目的考点，
//     但保留了"单一 std::mutex 保护 stderr 输出"这个最小正确性保证；
//   * 用 ostringstream 拼装，代价是每条日志一次堆分配。生产级实现会用
//     栈上固定缓冲 + itoa，本项目以可读性优先。
#ifndef MINET_LOGGING_H
#define MINET_LOGGING_H

#include <sstream>
#include <string>

namespace net {

class Logger {
public:
    enum LogLevel { TRACE, DEBUG, INFO, WARN, ERROR, FATAL, NUM_LOG_LEVELS };

    Logger(const char* file, int line, LogLevel level);
    ~Logger();

    // 低于当前级别的日志写入 sink_（黑洞流），既不拼字符串也不输出。
    // 这样宏本身是一个完整表达式，不会出现 if/else 悬挂匹配的问题。
    std::ostringstream& stream() { return enabled_ ? stream_ : sink_; }

    static void setLogLevel(LogLevel level);
    static LogLevel logLevel();

private:
    std::ostringstream stream_;
    std::ostringstream sink_;
    LogLevel level_;
    const char* file_;
    int line_;
    bool enabled_;
};

}  // namespace net

// 用法：LOG_INFO << "conn " << name << " closed";
#define LOG_TRACE ::net::Logger(__FILE__, __LINE__, ::net::Logger::TRACE).stream()
#define LOG_DEBUG ::net::Logger(__FILE__, __LINE__, ::net::Logger::DEBUG).stream()
#define LOG_INFO  ::net::Logger(__FILE__, __LINE__, ::net::Logger::INFO).stream()
#define LOG_WARN ::net::Logger(__FILE__, __LINE__, ::net::Logger::WARN).stream()
#define LOG_ERROR ::net::Logger(__FILE__, __LINE__, ::net::Logger::ERROR).stream()
#define LOG_FATAL ::net::Logger(__FILE__, __LINE__, ::net::Logger::FATAL).stream()

#endif  // MINET_LOGGING_H
