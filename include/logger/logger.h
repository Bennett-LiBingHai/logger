#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "logger/config.h"
#include "logger/field.h"
#include "logger/format.h"
#include "logger/formatter/json_formatter.h"
#include "logger/formatter/text_formatter.h"
#include "logger/record.h"
#include "logger/sink.h"

// 根据配置选择格式化器
inline FormatResult format_record(const Record& msg, const LogConfig& config, bool less) {
  if (config.format == LogFormat::JSON)
    return JsonFormatter::format(msg, config, less);
  return TextFormatter::format(msg, config, less);
}

// 日志器
class Logger {
 public:
  // 获取实例
  [[nodiscard]] static Logger& get_instance();

  // 打印日志带格式（宏入口，自带文件/行/函数）
  template <typename... Args>
  void log(LogLevel logLevel, const char* file, int line, const char* func, const char* fmt,
           Args&&... args) noexcept {
    log_impl(logLevel, file, line, func, fmt, false, std::forward<Args>(args)...);
  }

  // 打印日志带格式（直接调用，不带文件/行/函数）
  template <typename... Args>
  void log(LogLevel logLevel, const char* fmt, Args&&... args) noexcept {
    log_impl(logLevel, nullptr, 0, nullptr, fmt, true, std::forward<Args>(args)...);
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

  // 追加字段：空 key 跳过
  void append_field(std::vector<Field>& fields, Field f);

  // 递归终止
  std::tuple<> split_fields(std::vector<Field>& /*fields*/);

  // 拆分可变参数：Field 进 fields（保持顺序），其余进 tuple 作为位置参数（保持顺序）
  template <typename T, typename... Rest>
  auto split_fields(std::vector<Field>& fields, T&& first, Rest&&... rest);

  // 字段去重：同 key 后写覆盖（保留最后一个值），位置取首次出现，保持顺序
  void dedup_fields(std::vector<Field>& fields);

  // 后台线程主循环
  void async_loop();

  // 后台线程写一条（快照 config/sinks 后无锁写）
  void write_one(LogData data);

  // 移除队列级别最低的一条日志，并压入新日志（原子，同队列锁内完成）
  void drop_debug(LogData&& data);

  // log_impl的同步分支：config/sinks 已由调用方快照，本函数只负责无锁格式化 + 窄锁写 sink
  void log_impl_sync(const Record& msg, const LogConfig& config,
                     const std::vector<std::shared_ptr<LogSink>>& sinks, bool less);

  Logger() = default;

  // log前分流
  template <typename... Args>
  void log_impl(LogLevel logLevel, const char* file, int line, const char* func, const char* fmt,
                bool less, Args&&... args) noexcept;

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

// log前分流
template <typename... Args>
void Logger::log_impl(LogLevel logLevel, const char* file, int line, const char* func,
                      const char* fmt, bool less, Args&&... args) noexcept {
  // 1. 级别过滤（lock-free 早退）+ 一次持锁快照字段/config/sinks
  if (logLevel < impl_->log_level.load(std::memory_order_relaxed))
    return;

  std::vector<Field> fields;
  LogConfig config;
  std::vector<std::shared_ptr<LogSink>> sinks;
  bool is_async;
  {
    std::unique_lock<std::mutex> lock(impl_->mtx);
    fields = fields_;
    config = impl_->config;
    is_async = impl_->async_running.load(std::memory_order_acquire);
    if (!is_async)
      sinks = impl_->sinks;  // 异步路径由后台线程自行快照，省掉这笔拷贝
  }

  // 2. 物化 Record（无锁，本地工作）
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
      case AsyQueFulStrategy::SyncFallback: {
        std::vector<std::shared_ptr<LogSink>> fb_sinks;
        {
          std::unique_lock<std::mutex> lock(impl_->mtx);
          fb_sinks = impl_->sinks;
        }
        log_impl_sync(msg, config, fb_sinks, less);
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

// 拆分可变参数：Field 进 fields（保持顺序），其余进 tuple 作为位置参数（保持顺序）
template <typename T, typename... Rest>
auto Logger::split_fields(std::vector<Field>& fields, T&& first, Rest&&... rest) {
  if constexpr (std::is_same_v<std::decay_t<T>, Field>) {
    append_field(fields, std::forward<T>(first));  // 空 key 跳过
    return split_fields(fields, std::forward<Rest>(rest)...);
  } else {
    auto tail = split_fields(fields, std::forward<Rest>(rest)...);
    return std::tuple_cat(std::forward_as_tuple(std::forward<T>(first)), std::move(tail));
  }
}
