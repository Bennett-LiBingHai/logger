#include "logger/logger.h"

// 获取实例
[[nodiscard]] Logger& Logger::get_instance() {
  static Logger logger;
  return logger;
}

// 根据配置选择格式化器
FormatResult Logger::format_record(const Record& msg, const LogConfig& config, bool less) {
  if (config.format == LogFormat::JSON)
    return JsonFormatter::format(msg, config, less);
  return TextFormatter::format(msg, config, less);
}

// 序列化并写入 sinks，返回 {写失败数, 丢弃数},不持锁
std::pair<unsigned long long, unsigned long long> write_to_sinks(
    const FormatResult& result, const LogConfig& config,
    const std::vector<std::shared_ptr<LogSink>>& sinks) {
  unsigned long long failed = 0;
  unsigned long long dropped = 0;
  for (auto& sink : sinks) {
    bool ok = false;
    try {
      ok = sink->log(result);
    } catch (...) {
      ok = false;  // sink 抛异常视为失败，不允许日志导致进程崩溃
    }
    if (!ok) {
      ++failed;
      if (config.log_fail_strategy == LogFailStrategy::FallbackToStderr) {
        std::cerr << result;
      } else {
        ++dropped;
      }
    }
  }
  return {failed, dropped};
}

// 析构：停止异步线程并刷盘
Logger::~Logger() {
  close();
}

// 主动关闭日志器,返回统计信息
LogStats Logger::close() {
  impl_->stop.store(true, std::memory_order_release);
  impl_->cv.notify_all();
  if (impl_->async_thread.joinable())
    impl_->async_thread.join();
  return stats();
}

// 增加输出槽
void Logger::add_sink(std::shared_ptr<LogSink> log_sink) {
  std::unique_lock<std::mutex> lock(impl_->mtx);
  impl_->sinks.emplace_back(std::move(log_sink));
}

// 修改配置（异步惰性启动后台线程）
void Logger::set_config(const LogConfig& config) {
  bool need_start = false;
  {
    std::unique_lock<std::mutex> lock(impl_->mtx);
    impl_->config = config;
    impl_->log_level.store(config.log_level, std::memory_order_relaxed);
  }
  if (config.async && !impl_->async_running.load(std::memory_order_acquire)) {
    impl_->async_running.store(true, std::memory_order_release);
    need_start = true;
  }
  if (!config.async && impl_->async_running.load(std::memory_order_acquire)) {
    std::unique_lock<std::mutex> lock(impl_->mtx);
    impl_->config.async = true;
  }
  if (need_start) {
    impl_->async_thread = std::thread([this] { async_loop(); });
  }
}

// 获取配置
LogConfig Logger::get_config() {
  std::unique_lock<std::mutex> lock(impl_->mtx);
  return impl_->config;
}

// 刷新所有日志缓冲区（异步模式下等待队列清空并写完）
void Logger::flush_all() {
  if (impl_->async_running.load(std::memory_order_acquire)) {
    std::unique_lock<std::mutex> qlock(impl_->queue_mtx);
    impl_->cv.wait(qlock, [&] { return impl_->queue.empty() && !impl_->in_flight; });
  }
  std::unique_lock<std::mutex> lock(impl_->mtx);
  for (auto& sink : impl_->sinks)
    sink->flush();
}

// 获取日志自身统计
[[nodiscard]] LogStats Logger::stats() {
  std::unique_lock<std::mutex> lock(impl_->mtx);
  return impl_->stats;
}

// 后台线程主循环
void Logger::async_loop() {
  for (;;) {
    LogData data;
    {
      std::unique_lock<std::mutex> qlock(impl_->queue_mtx);
      impl_->cv.wait(qlock, [&] {
        return !impl_->queue.empty() || impl_->stop.load(std::memory_order_acquire);
      });
      if (impl_->queue.empty()) {  // stop一定为true
        break;
      }
      data = std::move(impl_->queue.front());
      impl_->queue.pop_front();
      impl_->in_flight.store(true, std::memory_order_release);
    }
    impl_->ful_cv.notify_one();
    write_one(std::move(data));
    std::unique_lock<std::mutex> qlock(impl_->queue_mtx);
    impl_->in_flight.store(false, std::memory_order_release);
    impl_->cv.notify_all();  // 唤醒 flush_all 等待者
  }
  // 退出前刷新所有 sink
  std::unique_lock<std::mutex> lock(impl_->mtx);
  for (auto& sink : impl_->sinks)
    sink->flush();
}

// 后台线程写一条（快照 config/sinks 后无锁写）
void Logger::write_one(LogData data) {
  std::vector<std::shared_ptr<LogSink>> sinks;
  LogConfig config;
  {
    std::unique_lock<std::mutex> lock(impl_->mtx);
    config = impl_->config;
    sinks = impl_->sinks;
  }
  FormatResult result = format_record(data.msg, config, data.less);
  // 超过长度自动截断
  if (result.formatted_msg.size() > config.max_log_item_size) {
    result.formatted_msg.resize(config.max_log_item_size);
  }
  auto [failed, dropped] = write_to_sinks(result, config, sinks);
  if (failed || dropped) {
    std::unique_lock<std::mutex> lock(impl_->mtx);
    impl_->stats.failed_writes += failed;
    impl_->stats.dropped += dropped;
  }
}

// 移除队列级别最低的一条日志，并压入新日志
void Logger::drop_debug(LogData&& data) {
  std::vector<LogLevel> v{LogLevel::TRACE, LogLevel::DEBUG, LogLevel::INFO,
                          LogLevel::WARN,  LogLevel::ERROR, LogLevel::FATAL};
  std::unique_lock<std::mutex> qlock(impl_->queue_mtx);
  for (auto& level : v)
    for (auto i = impl_->queue.rbegin(); i != (impl_->queue.rend()); ++i) {
      if ((*i).msg.log_level <= level) {
        impl_->queue.erase(--(i.base()));
        impl_->queue.push_back(std::move(data));
        return;
      }
    }
  // 队列已满但没有任何可丢弃的（理论上不可达，防御性兜底）
  impl_->queue.push_back(std::move(data));
}

// log_impl的同步分支：config/sinks 已由调用方快照，本函数只负责无锁格式化 + 窄锁写 sink
void Logger::log_impl_sync(const Record& msg, const LogConfig& config,
                           const std::vector<std::shared_ptr<LogSink>>& sinks, bool less) {
  FormatResult result = format_record(msg, config, less);
  // 超过长度自动截断
  if (result.formatted_msg.size() > config.max_log_item_size) {
    result.formatted_msg.resize(config.max_log_item_size);
  }

  // 写 sink 需要互斥（FileSink/ConsoleSink 非线程安全），但格式化已在锁外完成
  std::unique_lock<std::mutex> lock(impl_->mtx);
  auto [failed, dropped] = write_to_sinks(result, config, sinks);
  impl_->stats.failed_writes += failed;
  impl_->stats.dropped += dropped;
}
