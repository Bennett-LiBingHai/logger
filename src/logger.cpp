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

// 获取日志自身统计
[[nodiscard]] LogStats Logger::stats() {
  std::unique_lock<std::mutex> lock(impl_->mtx);
  return impl_->stats;
}

// 追加字段：空 key 跳过
void Logger::append_field(std::vector<Field>& fields, Field f) {
  if (!f.key.empty())
    fields.emplace_back(std::move(f));
}

// 递归终止
std::tuple<> Logger::split_fields(std::vector<Field>& /*fields*/) {
  return {};
}

// 字段去重：同 key 后写覆盖（保留最后一个值），位置取首次出现，保持顺序
void Logger::dedup_fields(std::vector<Field>& fields) {
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
