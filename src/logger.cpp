#include "logger/logger.h"

// 获取实例
[[nodiscard]] Logger& Logger::get_instance() {
  static Logger logger;
  return logger;
}

// 打印日志纯字符串
void Logger::log(LogLevel logLevel, const char* file, int line, const char* func, const char* str) {
  std::unique_lock<std::mutex> lock(mtx_);
  if (logLevel < config_.log_level)
    return;
  char buf[config_.max_log_item_size + 1];
  snprintf(buf, sizeof(buf), "%s", str);  // 超过长度会自动截断并追加'\0'
  Record msg{std::chrono::system_clock::now(),
             logLevel,
             buf,
             std::this_thread::get_id(),
             file,
             line,
             func};
  FormatResult result = TextFormatter::format(msg, config_);
  // 超过长度自动截断
  if (result.formatted_msg.size() > config_.max_log_item_size) {
    result.formatted_msg.resize(config_.max_log_item_size);
  }
  for (auto& sink : sinks_) {
    sink->log(result);
  }
}

// 增加输出槽
void Logger::add_sink(std::shared_ptr<LogSink> log_sink) {
  std::unique_lock<std::mutex> lock(mtx_);
  sinks_.emplace_back(std::move(log_sink));
}

// 修改配置
void Logger::set_config(const LogConfig& config) {
  std::unique_lock<std::mutex> lock(mtx_);
  config_ = config;
}

// 获取配置
LogConfig Logger::get_config() {
  std::unique_lock<std::mutex> lock(mtx_);
  return config_;
}

// 刷新所有日志缓冲区
void Logger::flush_all() {
  std::unique_lock<std::mutex> lock(mtx_);
  for (auto& sink : sinks_) {
    sink->flush();
  }
}
