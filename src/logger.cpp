#include "logger/logger.h"

// 获取实例
[[nodiscard]] Logger& Logger::get_instance() {
  static Logger logger;
  return logger;
}

// 增加输出槽
void Logger::add_sink(std::shared_ptr<LogSink> log_sink) {
  std::unique_lock<std::mutex> lock(impl_->mtx);
  impl_->sinks.emplace_back(std::move(log_sink));
}

// 修改配置
void Logger::set_config(const LogConfig& config) {
  std::unique_lock<std::mutex> lock(impl_->mtx);
  impl_->config = config;
}

// 获取配置
LogConfig Logger::get_config() {
  std::unique_lock<std::mutex> lock(impl_->mtx);
  return impl_->config;
}

// 刷新所有日志缓冲区
void Logger::flush_all() {
  std::unique_lock<std::mutex> lock(impl_->mtx);
  for (auto& sink : impl_->sinks) {
    sink->flush();
  }
}
