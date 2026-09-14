#include "logger/logger.h"

// Logger 非模板部分的实现：生命周期、配置、后台线程、写出与各处理阶段。

// 获取全局单例。首次调用时构造，进程退出时析构（析构会 flush 并停止后台线程）
[[nodiscard]] Logger& Logger::get_instance() {
  static Logger logger;
  return logger;
}

// 按配置的格式选择对应 Formatter 并格式化。
// less=true 表示结构化入口，不输出文件/行号/函数
SinkInput Logger::format_record(const Record& msg, const LogConfig& config, bool less) {
  if (config.format == LogFormat::JSON)
    return JsonFormatter::format(msg, config, less);
  return TextFormatter::format(msg, config, less);
}

// 一次向全部 Sink 写出后的结果汇总
struct SinkWriteResult {
  unsigned long long ok = 0;       // 成功写入的 Sink 数
  unsigned long long failed = 0;   // 写失败的 Sink 数
  unsigned long long dropped = 0;  // 按失败策略丢弃的次数
};

// 依次写入全部 Sink。调用方需保证写入互斥（Sink 本身非线程安全）。
// 每个 Sink 单独 try —— 那是"这个 Sink 挂了继续试下一个"，与"整条失败即放弃"
// 粒度不同，所以不并入上层 log_impl 的统一兜底
SinkWriteResult write_to_sinks(const SinkInput& result, const LogConfig& config,
                               const std::vector<std::shared_ptr<LogSink>>& sinks) {
  SinkWriteResult r;
  for (auto& sink : sinks) {
    bool ok = false;
    try {
      ok = sink->log(result);
    } catch (...) {
      ok = false;  // sink 抛异常视为失败，不允许日志导致进程崩溃
    }
    if (ok) {
      ++r.ok;
    } else {
      ++r.failed;
      if (config.log_fail_strategy == LogFailStrategy::FallbackToStderr) {
        std::cerr << result;  // 降级到 stderr，保证 Error 级日志不因 Sink 故障而消失
      } else {
        ++r.dropped;
      }
    }
  }
  return r;
}

// 析构：仅状态所有者（单例）关闭日志器。
// 子 Logger 与拷贝共享同一份状态，若它们的析构也调 close()，
// 一个临时子对象离开作用域就会把整个日志器关掉（队列不再有人消费）
Logger::~Logger() {
  if (owner_)
    close();
}

// 关闭日志器：停止接收、等队列消费完、回收后台线程，返回统计快照。
// 多线程同时调用是允许的：只让一个线程真正停止并 join，其余在 call_once 上等待。
// 直接对同一个 std::thread 并发调 joinable()/join() 是数据竞争（UB）
LogStats Logger::close() {
  std::call_once(impl_->close_once, [this] {
    // stop 同时充当"不再接收"：置位后 log_impl 直接丢弃，不再有记录进入队列。
    // 又因为它在 join 之前置位，生产者不可能在后台线程退出后才把记录塞进队列
    impl_->stop.store(true, std::memory_order_release);
    impl_->cv.notify_all();
    if (impl_->async_thread.joinable())
      impl_->async_thread.join();
    // 后台线程已退出，不会再有人腾空间：唤醒仍阻塞在队列满上的生产者，
    // 让它们看到 stop 后返回（否则会一直等下去）
    impl_->ful_cv.notify_all();
  });
  return stats();
}

// 增加输出目标。可挂多个，同一条日志依次写入每一个
void Logger::add_sink(std::shared_ptr<LogSink> log_sink) {
  std::unique_lock<std::mutex> lock(impl_->mtx);
  impl_->sinks.emplace_back(std::move(log_sink));
}

// 整份替换配置，并在需要且尚未启动时惰性启动后台线程
void Logger::set_config(const LogConfig& config) {
  bool need_start = false;
  {
    std::unique_lock<std::mutex> lock(impl_->mtx);
    impl_->config = config;
    impl_->log_level.store(config.log_level, std::memory_order_relaxed);  // 同步热缓存
  }
  if (config.async && !impl_->async_running.load(std::memory_order_acquire)) {
    impl_->async_running.store(true, std::memory_order_release);
    need_start = true;
  }
  // 异步一旦开启不可回退：线程无法安全地停了再启，且"线程还在但不再消费队列"
  // 比维持异步更糟。这里把配置里的 async 强制为 true，与新调用者的意图保持一致
  if (!config.async && impl_->async_running.load(std::memory_order_acquire)) {
    std::unique_lock<std::mutex> lock(impl_->mtx);
    impl_->config.async = true;
  }
  if (need_start) {
    impl_->async_thread = std::thread([this] { async_loop(); });
  }
}

// 只调整日志级别，其余配置不变，立即对热路径生效。
// 同时更新配置与热缓存原子；只对新记录生效，已构造/已入队的记录持各自的快照
void Logger::set_level(LogLevel level) {
  std::unique_lock<std::mutex> lock(impl_->mtx);
  impl_->config.log_level = level;
  impl_->log_level.store(level, std::memory_order_relaxed);
}

// 获取当前配置的一份快照（纯值结构，可安全跨线程持有）
LogConfig Logger::get_config() {
  std::unique_lock<std::mutex> lock(impl_->mtx);
  return impl_->config;
}

// 刷新：补发本线程挂起的聚合摘要，等队列清空，再刷新所有 Sink 的缓冲
void Logger::flush_all() {
  flush_dedup();

  if (impl_->async_running.load(std::memory_order_acquire)) {
    std::unique_lock<std::mutex> qlock(impl_->queue_mtx);
    impl_->cv.wait(qlock, [&] { return impl_->queue.empty() && !impl_->in_flight; });
  }
  std::unique_lock<std::mutex> lock(impl_->mtx);
  for (auto& sink : impl_->sinks)
    sink->flush();
}

// 获取自身统计快照。计数走原子 relaxed 读，不加锁；
// 只有队列长度需要短暂持队列锁 —— std::deque::size() 与 push/pop 并发是数据竞争
[[nodiscard]] LogStats Logger::stats() {
  const Counters& c = impl_->counters;
  LogStats s;
  s.written = c.written.load(std::memory_order_relaxed);
  s.failed_writes = c.failed_writes.load(std::memory_order_relaxed);
  s.dropped = c.dropped.load(std::memory_order_relaxed);
  s.dedup_suppressed = c.dedup_suppressed.load(std::memory_order_relaxed);
  s.masked_fields = c.masked_fields.load(std::memory_order_relaxed);
  s.queue_peak = c.queue_peak.load(std::memory_order_relaxed);
  s.max_write_latency_us = c.max_write_latency_us.load(std::memory_order_relaxed);
  for (std::size_t i = 0; i < 7; ++i)
    s.by_level[i] = c.by_level[i].load(std::memory_order_relaxed);

  {
    std::unique_lock<std::mutex> qlock(impl_->queue_mtx);
    s.queue_length = impl_->queue.size();
  }
  return s;
}

// 后台线程主循环：取队列 → 写出 → 复位 in_flight
void Logger::async_loop() {
  for (;;) {
    LogData data;
    {
      std::unique_lock<std::mutex> qlock(impl_->queue_mtx);
      impl_->cv.wait(qlock, [&] {
        return !impl_->queue.empty() || impl_->stop.load(std::memory_order_acquire);
      });
      if (impl_->queue.empty()) {  // 队列空且已置 stop
        break;
      }
      data = std::move(impl_->queue.front());
      impl_->queue.pop_front();
      impl_->in_flight.store(true, std::memory_order_release);
    }
    impl_->ful_cv.notify_one();  // 让出空间，唤醒可能被阻塞的生产者
    try {
      write_one(data);
    } catch (...) {
      // 本函数是线程入口，未捕获的异常会 terminate 整个进程，必须就地兜住。
      // 且不能让异常越过下面的 in_flight 复位 —— 否则 flush_all 会永久等待
      impl_->counters.dropped.fetch_add(1, std::memory_order_relaxed);
    }
    std::unique_lock<std::mutex> qlock(impl_->queue_mtx);
    impl_->in_flight.store(false, std::memory_order_release);
    impl_->cv.notify_all();  // 唤醒 flush_all 等待者
  }
  // 退出前刷新所有 sink，保证已写出的内容都落到设备上
  std::unique_lock<std::mutex> lock(impl_->mtx);
  for (auto& sink : impl_->sinks)
    sink->flush();
}

// 后台线程写一条：快照 config/sinks → 格式化 → 写各 Sink → 更新指标。
// 格式化抛出的异常直接向上抛，由 async_loop 的兜底丢弃并计数
void Logger::write_one(const LogData& data) {
  std::vector<std::shared_ptr<LogSink>> sinks;
  LogConfig config;
  {
    std::unique_lock<std::mutex> lock(impl_->mtx);
    config = impl_->config;
    sinks = impl_->sinks;
  }
  const SinkInput result = format_record_budgeted(data.msg, config, data.less);
  const SinkWriteResult r = write_to_sinks(result, config, sinks);

  // 异步写出延迟：业务线程打点（msg.time）到此刻写完
  const auto latency = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::system_clock::now() - data.msg.time);
  Counters& c = impl_->counters;
  const auto us = static_cast<unsigned long long>(latency.count());
  if (us > c.max_write_latency_us.load(std::memory_order_relaxed))
    c.max_write_latency_us.store(us, std::memory_order_relaxed);

  c.failed_writes.fetch_add(r.failed, std::memory_order_relaxed);
  c.dropped.fetch_add(r.dropped, std::memory_order_relaxed);
  if (r.ok > 0) {  // 至少一个 Sink 成功，才算这条写出去了
    c.written.fetch_add(1, std::memory_order_relaxed);
    c.by_level[static_cast<std::size_t>(data.msg.log_level)].fetch_add(1,
                                                                       std::memory_order_relaxed);
  }
}

// 敏感字段脱敏：按 key 调用脱敏函数，只有值真被改变才重建 FieldValue。
//
// 调用点在 log_impl 中「合并完全部字段（with / context / 显式 KV）之后、构造 Record 之前」：
// 异步模式下记录会先入队列，脱敏必须早于入队，队列里不该存明文；此时字段也已合并完毕，
// 上下文里的敏感字段一并覆盖。
//
// 脱敏发生在编码之前：脱敏函数拿到的是未编码的原始值、返回的也是原始值，由 Formatter
// 统一决定加引号 / 转义，因此无需区分 text 与 JSON。
//
// 只有值真被改过才重建 —— 否则 KV("user_id", 2001) 会从 JSON 数字退化成字符串 "2001"。
//
// 脱敏函数抛出的异常直接向上抛，由 log_impl 丢弃整条：绝不"保留原值继续输出"，
// 那等于把敏感数据原样写进日志
void Logger::mask_sensitive_fields(std::vector<Field>& fields, const LogConfig& config) {
  if (!config.enable_sensitive_field_mask || !config.sensitive_field_masker)
    return;

  for (auto& f : fields) {
    std::string raw;
    f.value.encode(raw, false);  // 未编码的原始值
    const std::string masked = config.sensitive_field_masker(f.key, raw);
    if (masked != raw) {
      f.value = FieldValue::from(masked);
      impl_->counters.masked_fields.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

// 按 max_record_size 削减后格式化。
//
// 削减顺序：先丢末尾字段（保住消息正文），字段丢完仍超则按超出量截断消息。
// 最少形态是「框架 + "..."」—— 框架（时间戳/级别/文件行号）必输出，切了整条记录无法解析。
// 绝不对格式化结果做字节级硬切：那会切掉行尾换行、切出非法 JSON
SinkInput Logger::format_record_budgeted(const Record& msg, const LogConfig& config, bool less) {
  SinkInput result = format_record(msg, config, less);
  if (config.max_record_size == 0 || result.formatted_msg.size() <= config.max_record_size)
    return result;

  // 先算出最少形态（丢光字段、消息只留省略标记），预算不可满足时直接用它
  Record minimal = msg;
  minimal.fields.clear();
  minimal.content = "...";
  const SinkInput minimal_result = format_record(minimal, config, less);

  // 丢末尾字段：每次重排一条，字段数很少，代价可接受
  Record trimmed = msg;
  while (!trimmed.fields.empty() && result.formatted_msg.size() > config.max_record_size) {
    trimmed.fields.pop_back();
    result = format_record(trimmed, config, less);
  }

  // 字段已丢完仍超：按超出量截断消息（转义会改变长度，此处一次逼近即可）
  if (result.formatted_msg.size() > config.max_record_size) {
    const std::size_t over = result.formatted_msg.size() - config.max_record_size;
    const std::size_t keep = trimmed.content.size() > over ? trimmed.content.size() - over : 0;
    if (keep < 3)
      trimmed.content = "...";  // 消息被削到无内容，留省略标记
    else
      truncate_utf8(trimmed.content, keep);
    result = format_record(trimmed, config, less);
  }

  // 仍超说明预算小于「框架 + ...」，无法满足：给出最少形态
  return result.formatted_msg.size() > config.max_record_size ? minimal_result : result;
}

// 按 max_field_length 截断超长字段值，只对确实超长的字段重建 FieldValue。
//
// 按目标格式渲染后度量 —— 同一个值在 text 与 JSON 下长度不同
// （如 double 的 NaN 是 "nan" 对 "null"）
void Logger::limit_field_lengths(std::vector<Field>& fields, const LogConfig& config) {
  if (config.max_field_length == 0)
    return;

  const bool json = (config.format == LogFormat::JSON);
  for (auto& f : fields) {
    std::string value;
    f.value.encode(value, json);
    if (value.size() > config.max_field_length) {
      truncate_utf8(value, config.max_field_length);
      f.value = FieldValue::from(std::move(value));
    }
  }
}

// 当前线程待补发的聚合载荷：序列首次重复时组装（含上下文/堆栈/字段），序列结束时输出
Record& Logger::pending_dedup_payload() {
  static thread_local Record payload;
  return payload;
}

// 载荷是否已成功组装。组装中途抛异常时保持 false，避免发出空记录
bool& Logger::pending_dedup_ready() {
  static thread_local bool ready = false;
  return ready;
}

// 补发聚合摘要：取走载荷，填入重复次数后输出
void Logger::emit_dedup_summary(std::uint64_t count, const LogConfig& config, bool is_async,
                                const std::vector<std::shared_ptr<LogSink>>& sinks) {
  if (!pending_dedup_ready())
    return;  // 载荷没能组装出来（如消息编码失败），不发摘要
  pending_dedup_ready() = false;

  Record payload = std::move(pending_dedup_payload());
  pending_dedup_payload() = Record{};  // 载荷已取走，复位供下一条序列使用
  payload.repeat_count = count;
  payload.time = std::chrono::system_clock::now();  // 摘要是此刻产生的，用当前时间
  route_record(std::move(payload), config, is_async, sinks);
}

// 结束当前线程的去重序列并补发摘要，供 flush_all 调用。
// 其它线程的槽无法触及（thread_local），由它们各自的下一条不同日志收尾。
// 本路径不经过 log_impl 的兜底，自己接住异常：刷盘是尽力而为
void Logger::flush_dedup() noexcept {
  const std::uint64_t count = dedup_filter().take_prev_count();
  if (count <= 1)
    return;

  try {
    LogConfig config;
    std::vector<std::shared_ptr<LogSink>> sinks;
    bool is_async;
    {
      std::unique_lock<std::mutex> lock(impl_->mtx);
      config = impl_->config;
      is_async = impl_->async_running.load(std::memory_order_acquire);
      if (!is_async)
        sinks = impl_->sinks;
    }
    emit_dedup_summary(count, config, is_async, sinks);
  } catch (...) {
    impl_->counters.dropped.fetch_add(1, std::memory_order_relaxed);
  }
}

// DropDebug 策略：按级别从低到高，从队尾删掉最先找到的一条，再压入新记录。
// 调用方需持有 queue_mtx。被顶掉的那条计入 dropped
void Logger::drop_debug(LogData&& data) {
  std::vector<LogLevel> v{LogLevel::TRACE, LogLevel::DEBUG, LogLevel::INFO,
                          LogLevel::WARN,  LogLevel::ERROR, LogLevel::FATAL};
  std::unique_lock<std::mutex> qlock(impl_->queue_mtx);
  for (auto& level : v)
    for (auto i = impl_->queue.rbegin(); i != (impl_->queue.rend()); ++i) {
      if ((*i).msg.log_level <= level) {
        impl_->queue.erase(--(i.base()));
        impl_->queue.push_back(std::move(data));
        impl_->counters.dropped.fetch_add(1, std::memory_order_relaxed);
        return;
      }
    }
  // 队列已满但没有任何可丢弃的（理论上不可达，防御性兜底）
  impl_->queue.push_back(std::move(data));
}

// 同步分支：格式化后加窄锁写 Sink。
//
// 格式化在锁外完成（可能编码用户自定义类型，耗时不可控）；写 Sink 需要互斥，因为
// ConsoleSink / FileSink 本身不是线程安全的。
// 格式化抛出的异常直接向上抛，由 log_impl 的兜底统一丢弃并计数
void Logger::log_impl_sync(const Record& msg, const LogConfig& config,
                           const std::vector<std::shared_ptr<LogSink>>& sinks, bool less) {
  const SinkInput result = format_record_budgeted(msg, config, less);

  std::unique_lock<std::mutex> lock(impl_->mtx);
  const SinkWriteResult r = write_to_sinks(result, config, sinks);
  Counters& c = impl_->counters;
  c.failed_writes.fetch_add(r.failed, std::memory_order_relaxed);
  c.dropped.fetch_add(r.dropped, std::memory_order_relaxed);
  if (r.ok > 0) {
    c.written.fetch_add(1, std::memory_order_relaxed);
    c.by_level[static_cast<std::size_t>(msg.log_level)].fetch_add(1, std::memory_order_relaxed);
  }
}
