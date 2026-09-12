#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "logger/config.h"
#include "logger/context.h"
#include "logger/error.h"
#include "logger/format.h"
#include "logger/formatter/json_formatter.h"
#include "logger/formatter/text_formatter.h"
#include "logger/record.h"
#include "logger/sink.h"
#include "logger/stacktrace.h"
#include "logger/trace.h"

// 日志器
class Logger {
 public:
  // 获取实例
  [[nodiscard]] static Logger& get_instance();

  // 打印日志带格式（宏入口，自带文件/行/函数）
  template <typename... Args>
  void log(LogLevel logLevel, const char* file, int line, const char* func, const char* fmt,
           Args&&... args) noexcept {
    log_impl(logLevel, file, line, func, fmt, std::forward<Args>(args)...);
  }

  // 打印日志带格式（直接调用，不带文件/行/函数）
  template <typename... Args>
  void log(LogLevel logLevel, const char* fmt, Args&&... args) noexcept {
    log_impl(logLevel, nullptr, 0, nullptr, fmt, std::forward<Args>(args)...);
  }

  // 打印trace级别日志
  template <typename... Args>
  void trace(const char* fmt, Args&&... args) noexcept {
    log(LogLevel::TRACE, fmt, std::forward<Args>(args)...);
  }
  // 打印debug级别日志
  template <typename... Args>
  void debug(const char* fmt, Args&&... args) noexcept {
    log(LogLevel::DEBUG, fmt, std::forward<Args>(args)...);
  }
  // 打印info级别日志
  template <typename... Args>
  void info(const char* fmt, Args&&... args) noexcept {
    log(LogLevel::INFO, fmt, std::forward<Args>(args)...);
  }
  // 打印warn级别日志
  template <typename... Args>
  void warn(const char* fmt, Args&&... args) noexcept {
    log(LogLevel::WARN, fmt, std::forward<Args>(args)...);
  }
  // 打印error级别日志
  template <typename... Args>
  void error(const char* fmt, Args&&... args) noexcept {
    log(LogLevel::ERROR, fmt, std::forward<Args>(args)...);
  }
  // 打印fatal级别日志
  template <typename... Args>
  void fatal(const char* fmt, Args&&... args) noexcept {
    log(LogLevel::FATAL, fmt, std::forward<Args>(args)...);
  }

  // 记录异常：自动展开为 error / error_type / error_chain 字段（LOG_EXCEPTION 宏入口）
  // 额外 KV 字段照常附加，堆栈不是特例：
  //   LOG_EXCEPTION("db failed", e)
  //   LOG_EXCEPTION("db failed", e, KV("stacktrace", StackTrace::capture()))
  template <typename... Args>
  void log_exception(LogLevel level, const char* file, int line, const char* func, const char* msg,
                     const std::exception& e, Args&&... args) noexcept;
  template <typename... Args>
  void log_exception(LogLevel level, const char* file, int line, const char* func, const char* msg,
                     const std::exception_ptr& e, Args&&... args) noexcept;

  // 结构化异常接口（不带文件/行/函数）
  void exception(const char* msg, const std::exception& e) noexcept {
    log_exception(LogLevel::ERROR, nullptr, 0, nullptr, msg, e);
  }
  void exception(const char* msg, const std::exception_ptr& e) noexcept {
    log_exception(LogLevel::ERROR, nullptr, 0, nullptr, msg, e);
  }

  // 增加输出槽
  void add_sink(std::shared_ptr<LogSink> log_sink);

  // 更新配置
  void set_config(const LogConfig& config);

  // 获取配置
  LogConfig get_config();

  // 刷新所有日志缓冲区（异步模式下等待队列清空并写完）
  void flush_all();

  // 主动关闭日志器,返回统计信息
  LogStats close();

  // 获取日志自身统计
  [[nodiscard]] LogStats stats();

  // 预绑定字段：共享 sinks/config/mutex，返回带额外字段的子 Logger
  template <typename... Args>
  Logger with(Args&&... args);

  // 统一业务字段：命名固定的字段，避免各处手写 key 拼错（M5）
  Logger with_request_id(std::string_view id);
  Logger with_trace_id(std::string_view id);
  Logger with_span_id(std::string_view id);
  Logger with_user_id(std::string_view id);
  Logger with_service(std::string_view name);
  Logger with_trace(const TraceContext& ctx);  // 一次带上 trace_id / span_id / trace_flags

  // 析构：停止异步线程并刷盘
  ~Logger();

 private:
  // 待打印的日志信息
  struct LogData {
    Record msg;
    bool less;
  };

  // logger共享资源
  struct Impl {
    std::mutex mtx;  // 保护 sinks/config/stats/async_running
    std::vector<std::shared_ptr<LogSink>> sinks;
    LogConfig config;
    std::atomic<LogLevel> log_level{LogLevel::TRACE};  // 级别热缓存，供热路径 lock-free 早退
    LogStats stats;
    std::atomic<bool> async_running{false};  // 异步线程是否已启动（启动后不变）

    std::mutex queue_mtx;  // 保护 queue/stop/in_flight
    std::condition_variable cv;
    std::condition_variable ful_cv;  // 用于队列满时的阻塞策略
    std::deque<LogData> queue;
    std::atomic<bool> stop{false};
    std::atomic<bool> in_flight{false};  // 后台线程是否正在写一条

    std::thread async_thread;
  };

  // 后台线程主循环
  void async_loop();

  // 后台线程写一条（快照 config/sinks 后无锁写）
  void write_one(LogData data);

  // 移除队列级别最低的一条日志，并压入新日志（原子，同队列锁内完成）
  void drop_debug(LogData&& data);

  // log_impl的同步分支：config/sinks 已由调用方快照，本函数只负责无锁格式化 + 窄锁写 sink
  void log_impl_sync(const Record& msg, const LogConfig& config,
                     const std::vector<std::shared_ptr<LogSink>>& sinks, bool less);

  // log前分流
  template <typename... Args>
  void log_impl(LogLevel logLevel, const char* file, int line, const char* func, const char* fmt,
                Args&&... args) noexcept;

  // 异常字段展开后复用 log_impl；无嵌套时 error_chain 用空 key 占位，由 append_field 跳过
  template <typename... Args>
  void log_exception_impl(LogLevel level, const char* file, int line, const char* func,
                          const char* msg, const ExceptionInfo& info, Args&&... args) noexcept;

  // 根据配置选择格式化器
  FormatResult format_record(const Record& msg, const LogConfig& config, bool less);

  std::shared_ptr<Impl> impl_ = std::make_shared<Impl>();
  std::vector<Field> fields_;
};

// 获取文件名
constexpr const char* filename_of(const char* path) {
  const char* cur = path;
  while (*cur)
    ++cur;
  while (cur != path) {
    if (*cur == '/' || *cur == '\\')
      return cur + 1;
    --cur;
  }
  return path;
}

// 宏封装
#define __FILENAME__ filename_of(__FILE__)  // 获取文件名
#define LOG_TRACE(fmt, ...)                                                          \
  Logger::get_instance().log(LogLevel::TRACE, __FILENAME__, __LINE__, __func__, fmt, \
                             ##__VA_ARGS__)  // 细粒度调试
#define LOG_DEBUG(fmt, ...)                                                          \
  Logger::get_instance().log(LogLevel::DEBUG, __FILENAME__, __LINE__, __func__, fmt, \
                             ##__VA_ARGS__)  // 调试
#define LOG_INFO(fmt, ...)                                                          \
  Logger::get_instance().log(LogLevel::INFO, __FILENAME__, __LINE__, __func__, fmt, \
                             ##__VA_ARGS__)  // 信息
#define LOG_WARN(fmt, ...)                                                          \
  Logger::get_instance().log(LogLevel::WARN, __FILENAME__, __LINE__, __func__, fmt, \
                             ##__VA_ARGS__)  // 警告
#define LOG_ERROR(fmt, ...)                                                          \
  Logger::get_instance().log(LogLevel::ERROR, __FILENAME__, __LINE__, __func__, fmt, \
                             ##__VA_ARGS__)  // 错误
#define LOG_FATAL(fmt, ...)                                                          \
  Logger::get_instance().log(LogLevel::FATAL, __FILENAME__, __LINE__, __func__, fmt, \
                             ##__VA_ARGS__)  // 致命
#define LOG_EXCEPTION(msg, exc, ...)                                                             \
  Logger::get_instance().log_exception(LogLevel::ERROR, __FILENAME__, __LINE__, __func__, (msg), \
                                       (exc), ##__VA_ARGS__)  // 记录异常并自动展开字段

// 预绑定字段：共享 sinks/config/mutex，返回带额外字段的子 Logger
template <typename... Args>
Logger Logger::with(Args&&... args) {
  static_assert((std::is_same_v<std::decay_t<Args>, Field> && ...), "with() 仅接受 KV(...) 字段");
  std::unique_lock<std::mutex> lock(impl_->mtx);
  Logger child;
  child.impl_ = impl_;
  child.fields_ = fields_;
  (append_field(child.fields_, std::forward<Args>(args)), ...);
  return child;
}

// ===== 统一业务字段（M5）=====
// 命名固定的常用字段，避免调用方各处手写 key 拼错
inline Logger Logger::with_request_id(std::string_view id) {
  return with(KV("request_id", id));
}

inline Logger Logger::with_trace_id(std::string_view id) {
  return with(KV("trace_id", id));
}

inline Logger Logger::with_span_id(std::string_view id) {
  return with(KV("span_id", id));
}

inline Logger Logger::with_user_id(std::string_view id) {
  return with(KV("user_id", id));
}

inline Logger Logger::with_service(std::string_view name) {
  return with(KV("service", name));
}

inline Logger Logger::with_trace(const TraceContext& ctx) {
  return with(KV("trace_id", ctx.trace_id), KV("span_id", ctx.span_id),
              KV("trace_flags", ctx.trace_flags));
}

// log前分流
template <typename... Args>
void Logger::log_impl(LogLevel logLevel, const char* file, int line, const char* func,
                      const char* fmt, Args&&... args) noexcept {
  // 级别过滤（lock-free 早退）+ 一次持锁快照字段/config/sinks
  if (logLevel < impl_->log_level.load(std::memory_order_relaxed))
    return;

  std::vector<Field> fields;
  LogConfig config;
  std::vector<std::shared_ptr<LogSink>> sinks;
  bool is_async;
  bool stacktrace;
  {
    std::unique_lock<std::mutex> lock(impl_->mtx);
    fields = fields_;
    config = impl_->config;
    is_async = impl_->async_running.load(std::memory_order_acquire);
    if (!is_async)
      sinks = impl_->sinks;  // 异步路径由后台线程自行快照，省掉这笔拷贝
    stacktrace = config.stacktrace == StackTraceMode::ALWAYS ||
                 (config.stacktrace == StackTraceMode::FATAL && logLevel >= LogLevel::FATAL);
  }

  // 和并作用域数据
  ContextScope::merge_into(fields);

  // Fatal 自动采集堆栈（显式 KV("stacktrace", ...) 由调用方自行附加）
  if (stacktrace)
    append_field(fields, KV("stacktrace", StackTrace::capture(1, config.stacktrace_depth,
                                                              config.max_stacktrace_length)));

  // 物化 Record（无锁，本地工作）
  auto positional = split_fields(fields, std::forward<Args>(args)...);
  dedup_fields(fields);  // 同 key 后写覆盖
  std::string content = std::apply([&](auto&&... pa) { return format(fmt, pa...); }, positional);

  Record msg{std::chrono::system_clock::now(),
             logLevel,
             std::move(content),
             std::this_thread::get_id(),
             file,
             line,
             func,
             std::move(fields)};

  const bool less = (file == nullptr);

  // 3. 路由：异步入队 / 同步直写
  if (is_async) {
    bool full;
    {
      std::unique_lock<std::mutex> qlock(impl_->queue_mtx);
      full = (impl_->queue.size()) >= (config.buffer_size);
    }
    if (full) {
      switch (config.asy_que_ful_strategy) {
      case AsyQueFulStrategy::Block: {
        std::unique_lock<std::mutex> qlock(impl_->queue_mtx);
        impl_->ful_cv.wait(qlock,
                           [this, &config]() { return impl_->queue.size() < config.buffer_size; });
        impl_->queue.push_back(LogData{std::move(msg), less});
        break;
      }
      case AsyQueFulStrategy::DropNewest: {
        return;
      }
      case AsyQueFulStrategy::DropOldest: {
        std::unique_lock<std::mutex> qlock(impl_->queue_mtx);
        impl_->queue.pop_front();
        impl_->queue.push_back(LogData{std::move(msg), less});
        break;
      }
      case AsyQueFulStrategy::DropDebug: {
        drop_debug(LogData{std::move(msg), less});
        break;
      }
      default: {
        return;
      }
      }
    } else {
      std::unique_lock<std::mutex> qlock(impl_->queue_mtx);
      impl_->queue.push_back(LogData{std::move(msg), less});
    }
    impl_->cv.notify_one();
  } else {
    log_impl_sync(msg, config, sinks, less);
  }
}

// 异常字段展开后复用 log_impl：error / error_type 恒有，error_chain 仅在嵌套时产生
// （无嵌套时用空 key 占位，append_field 会跳过它）
template <typename... Args>
void Logger::log_exception_impl(LogLevel level, const char* file, int line, const char* func,
                                const char* msg, const ExceptionInfo& info,
                                Args&&... args) noexcept {
  Field chain;
  if (info.has_nested)
    chain = KV("error_chain", info.chain);

  log_impl(level, file, line, func, msg, KV("error", info.message), KV("error_type", info.type),
           std::move(chain), std::forward<Args>(args)...);
}

template <typename... Args>
void Logger::log_exception(LogLevel level, const char* file, int line, const char* func,
                           const char* msg, const std::exception& e, Args&&... args) noexcept {
  log_exception_impl(level, file, line, func, msg, extract_exception(e),
                     std::forward<Args>(args)...);
}

template <typename... Args>
void Logger::log_exception(LogLevel level, const char* file, int line, const char* func,
                           const char* msg, const std::exception_ptr& e, Args&&... args) noexcept {
  log_exception_impl(level, file, line, func, msg, extract_exception(e),
                     std::forward<Args>(args)...);
}
