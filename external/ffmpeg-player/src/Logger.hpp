#ifndef LOGGER_HPP
#define LOGGER_HPP

#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

namespace Logger {

// 日志级别
enum LogLevel {
  LOG_INFO = 0,
  LOG_WARNING = 1,
  LOG_ERROR = 2,
  LOG_FATAL = 3,
};

class Logger {
public:
  Logger(char ** /*argv*/) {
    color_enabled_ = true;
    log_to_stderr_ = true;
  }

  static Logger &instance() {
    static Logger logger(nullptr);
    return logger;
  }

  void set_color(bool enabled) { color_enabled_ = enabled; }
  void set_log_to_stderr(bool enabled) { log_to_stderr_ = enabled; }

  // 设置日志文件输出
  void set_log_file(const std::string &path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_stream_.is_open())
      file_stream_.close();
    file_stream_.open(path, std::ios::app);
  }

  void log(LogLevel severity, const char *file, int line,
           const std::string &message) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 1. 获取时间（时分秒）
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &tt);
#else
    localtime_r(&tt, &tm_buf);
#endif
    char time_str[9]; // "HH:MM:SS\0"
    std::strftime(time_str, sizeof(time_str), "%H:%M:%S", &tm_buf);

    // 2. 日志级别字符
    static const char level_chars[] = "IWEF";
    char level_char = level_chars[severity];

    // 3. 文件名（不含路径）
    const char *basename = extract_basename(file);

    // 4. 构造带颜色的输出（用于终端）
    if (log_to_stderr_) {
      const char *color_start = "";
      const char *color_end = "";
      if (color_enabled_) {
        switch (severity) {
        case LOG_INFO:
          color_start = "\033[32m";
          break;
        case LOG_WARNING:
          color_start = "\033[33m";
          break;
        case LOG_ERROR:
        case LOG_FATAL:
          color_start = "\033[31m";
          break;
        }
        color_end = "\033[0m";
      }
      std::cerr << color_start << "【 " << level_char << " " << time_str
                << " 】" << basename << ": " << line << color_end << "\t"
                << message << "\n";
    }

    // 5. 写入文件（无颜色）
    if (file_stream_.is_open()) {
      file_stream_ << "【 " << level_char << " " << time_str << " 】"
                   << basename << ": " << line << "\t" << message << "\n";
      file_stream_.flush();
    }

    // 6. FATAL 级别终止程序
    if (severity == LOG_FATAL) {
      std::abort();
    }
  }

private:
  static const char *extract_basename(const char *filepath) {
    const char *slash = std::strrchr(filepath, '/');
    if (!slash)
      slash = std::strrchr(filepath, '\\');
    return slash ? slash + 1 : filepath;
  }

  bool color_enabled_ = true;
  bool log_to_stderr_ = true;
  std::ofstream file_stream_;
  std::mutex mutex_;
};

// ============================================================
// 流式日志消息构造器
// ============================================================
class LogMessage {
public:
  LogMessage(LogLevel level, const char *file, int line)
      : level_(level), file_(file), line_(line) {}

  ~LogMessage() { Logger::instance().log(level_, file_, line_, stream_.str()); }

  std::ostringstream &stream() { return stream_; }

private:
  LogLevel level_;
  const char *file_;
  int line_;
  std::ostringstream stream_;
};

// ============================================================
// 条件日志（用于 CHECK 宏失败时）
// ============================================================
class LogMessageVoidify {
public:
  void operator&(std::ostream &) {}
};

} // namespace Logger

// ============================================================
// 宏定义 — 保持与 glog 类似的使用方式
// ============================================================

#define LOG(level)                                                             \
  Logger::LogMessage(Logger::LOG_##level, __FILE__, __LINE__).stream()

#define LOG_IF(level, condition)                                               \
  !(condition)                                                                 \
      ? (void)0                                                                \
      : Logger::LogMessageVoidify() &                                          \
            Logger::LogMessage(Logger::LOG_##level, __FILE__, __LINE__)        \
                .stream()

#define CHECK(condition)                                                       \
  if (!(condition))                                                            \
  Logger::LogMessage(Logger::LOG_FATAL, __FILE__, __LINE__).stream()           \
      << "CHECK failed: " #condition " "

#define CHECK_EQ(a, b) CHECK((a) == (b)) << "(" << (a) << " vs " << (b) << ") "
#define CHECK_NE(a, b) CHECK((a) != (b)) << "(" << (a) << " vs " << (b) << ") "
#define CHECK_LT(a, b) CHECK((a) < (b)) << "(" << (a) << " vs " << (b) << ") "
#define CHECK_LE(a, b) CHECK((a) <= (b)) << "(" << (a) << " vs " << (b) << ") "
#define CHECK_GT(a, b) CHECK((a) > (b)) << "(" << (a) << " vs " << (b) << ") "
#define CHECK_GE(a, b) CHECK((a) >= (b)) << "(" << (a) << " vs " << (b) << ") "

#define CHECK_NOTNULL(ptr)                                                     \
  ([](auto *p, const char *file, int line) -> decltype(p) {                    \
    if (p == nullptr) {                                                        \
      Logger::LogMessage(Logger::LOG_FATAL, file, line).stream()               \
          << "CHECK_NOTNULL(" #ptr ") failed.";                                \
    }                                                                          \
    return p;                                                                  \
  })((ptr), __FILE__, __LINE__)

// DLOG — 仅 Debug 模式下生效
#ifdef NDEBUG
#define DLOG(level)                                                            \
  true ? (void)0                                                               \
       : Logger::LogMessageVoidify() &                                         \
             Logger::LogMessage(Logger::LOG_##level, __FILE__, __LINE__)       \
                 .stream()
#else
#define DLOG(level) LOG(level)
#endif

// VLOG — 简单实现，忽略 level 控制
#ifndef VLOG
#define VLOG(level) LOG(INFO)
#endif

// LOG_EVERY_N — 每 N 次输出一次
#ifndef LOG_EVERY_N
#define LOG_EVERY_N(level, n)                                                  \
  static int LOG_EVERY_N_counter_##__LINE__ = 0;                               \
  if (++LOG_EVERY_N_counter_##__LINE__ % (n) == 0)                             \
  LOG(level)
#endif

#endif // LOGGER_HPP