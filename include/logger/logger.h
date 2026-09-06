#pragma once
#include <chrono>
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

namespace detail {
// 追加字段：空 key 跳过
inline void append_field(std::vector<Field>& fields, Field f) {
  if (!f.key.empty())
    fields.emplace_back(std::move(f));
}

// 递归终止
inline std::tuple<> split_fields(std::vector<Field>& /*fields*/) {
  return {};
}

// 拆分可变参数：Field 进 fields（保持顺序），其余进 tuple 作为位置参数（保持顺序）
template <typename T, typename... Rest>
auto split_fields(std::vector<Field>& fields, T&& first, Rest&&... rest) {
  if constexpr (std::is_same_v<std::decay_t<T>, Field>) {
    append_field(fields, std::forward<T>(first));  // 空 key 跳过
    return split_fields(fields, std::forward<Rest>(rest)...);
  } else {
    auto tail = split_fields(fields, std::forward<Rest>(rest)...);
    return std::tuple_cat(std::forward_as_tuple(std::forward<T>(first)), std::move(tail));
  }
}

// 字段去重：同 key 后写覆盖（保留最后一个值），位置取首次出现，保持顺序
inline void dedup_fields(std::vector<Field>& fields) {
  if (fields.size() < 2)
    return;
  std::vector<Field> out;
  out.reserve(fields.size());
  std::unordered_map<std::string, size_t> index;  // key → 在 out 中的位置
  for (auto& f : fields) {
    auto it = index.find(f.key);
    if (it == index.end()) {
      index.emplace(f.key, out.size());
      out.emplace_back(std::move(f));
    } else {
      out[it->second].value = std::move(f.value);  // 覆盖值，保留首次位置
    }
  }
  fields = std::move(out);
}
}  // namespace detail

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

  // 刷新所有日志缓冲区
  void flush_all();

  // 预绑定字段：共享 sinks/config/mutex，返回带额外字段的子 Logger
  template <typename... Args>
  Logger with(Args&&... args) {
    static_assert((std::is_same_v<std::decay_t<Args>, Field> && ...), "with() 仅接受 KV(...) 字段");
    Logger child;
    child.impl_ = impl_;
    child.fields_ = fields_;
    (detail::append_field(child.fields_, std::forward<Args>(args)), ...);
    return child;
  }

 private:
  struct Impl {
    std::mutex mtx;
    std::vector<std::shared_ptr<LogSink>> sinks;
    LogConfig config;
  };

  Logger() = default;

  template <typename... Args>
  void log_impl(LogLevel logLevel, const char* file, int line, const char* func, const char* fmt,
                bool less, Args&&... args) noexcept {
    std::unique_lock<std::mutex> lock(impl_->mtx);
    if (logLevel < impl_->config.log_level)
      return;

    // 合并预绑定字段，再拆分本次调用里的 Field 与位置参数
    std::vector<Field> fields = fields_;
    auto positional = detail::split_fields(fields, std::forward<Args>(args)...);
    detail::dedup_fields(fields);  // 同 key 后写覆盖
    std::string content = std::apply([&](auto&&... pa) { return format(fmt, pa...); }, positional);

    Record msg{std::chrono::system_clock::now(),
               logLevel,
               std::move(content),
               std::this_thread::get_id(),
               file,
               line,
               func,
               std::move(fields)};
    FormatResult result = format_record(msg, impl_->config, less);
    // 超过长度自动截断
    if (result.formatted_msg.size() > impl_->config.max_log_item_size) {
      result.formatted_msg.resize(impl_->config.max_log_item_size);
    }
    for (auto& sink : impl_->sinks) {
      sink->log(result);
    }
  }

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
