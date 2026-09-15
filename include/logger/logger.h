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
#include "logger/crash_handler.h"
#include "logger/detail/dedup.h"
#include "logger/detail/error.h"
#include "logger/detail/format.h"
#include "logger/detail/formatter/json_formatter.h"
#include "logger/detail/formatter/text_formatter.h"
#include "logger/detail/record.h"
#include "logger/literals.h"
#include "logger/sink.h"
#include "logger/sink/console_sink.h"
#include "logger/sink/file_sink.h"
#include "logger/stacktrace.h"
#include "logger/trace.h"
#include "logger/version.h"

namespace logger {
// 内部类型在本头文件内的短别名：它们定义在 detail/ 下，不随本库的兼容性承诺走
using detail::dedup_filter;
using detail::DedupFilter;
using detail::extract_exception;
using detail::Record;
using detail::truncate_utf8;

/// @file logger.h
/// @brief 库的统一入口：日志器单例、`LOG_*` 宏、运行期配置，以及全部公共接口。
///
/// **包含这一个头即可** —— 它是库的统一入口，其余公共头（级别、配置、字段、上下文、
/// 堆栈、Trace、崩溃处理器、Sink、版本号、字节字面量）都由它带进来。

/// @brief 日志器（进程内单例）。
///
/// 一条日志的完整链路：级别过滤 → 聚合去重判定 → 合并上下文字段 → 采集堆栈（可选）
/// → 字段拆分与去重 → 脱敏 → 长度控制 → 构造记录 → 路由（异步队列 / 同步直写）
/// → 格式化 → 写 Sink。
///
/// @par 线程安全
/// 所有公开接口都可从多线程并发调用。级别过滤走原子热缓存，关闭的级别在入口处
/// 无锁早退，不产生任何后续开销。
///
/// @par 异常
/// 日志代码路径不向业务抛异常。内部故障（内存不足、用户自定义 operator<< 或脱敏函数
/// 抛异常）按配置丢弃该条记录或降级到 stderr，并计入 LogStats。
///
/// @par 典型用法
/// @code
/// Logger::get_instance().add_sink(std::make_shared<ConsoleSink>());
/// LOG_INFO("user {} login", user_id);
/// @endcode
class Logger {
 public:
  /// @brief 获取全局单例。
  /// @return 单例引用。首次调用时构造；进程退出时析构，析构会自动 flush 并停止后台线程。
  [[nodiscard]] static Logger& get_instance();

  /// @brief 打印一条日志（宏入口，自动携带文件 / 行号 / 函数名）。
  /// @param logLevel 日志级别；低于当前配置级别的会被静默丢弃。
  /// @param file 源文件名，由 LOG_* 宏传入 ::logger::filename_of(__FILE__)。
  /// @param line 源文件行号。
  /// @param func 所在函数名。
  /// @param fmt 格式串，用 {} 作占位符；参数不足时占位符原样保留。
  /// @param args 与 {} 依次对应的位置参数；Field 类型（即 KV(...)）作为结构化字段。
  /// @note 位置参数经用户自定义类型的 operator<< 编码，其抛出的异常会被兜住并丢弃该条。
  template <typename... Args>
  void log(LogLevel logLevel, const char* file, int line, const char* func, const char* fmt,
           Args&&... args) noexcept {
    log_impl(logLevel, file, line, func, fmt, std::forward<Args>(args)...);
  }

  /// @brief 打印一条日志（结构化入口，不携带文件 / 行号 / 函数名）。
  /// @param logLevel 日志级别。
  /// @param fmt 格式串，用 {} 作占位符。
  /// @param args 位置参数与 KV(...) 字段。
  template <typename... Args>
  void log(LogLevel logLevel, const char* fmt, Args&&... args) noexcept {
    log_impl(logLevel, nullptr, 0, nullptr, fmt, std::forward<Args>(args)...);
  }

  /// @brief 打印 TRACE 级日志（极细粒度执行过程，生产通常关闭）。
  /// @param fmt 格式串，用 {} 作占位符。
  /// @param args 位置参数与 KV(...) 字段。
  template <typename... Args>
  void trace(const char* fmt, Args&&... args) noexcept {
    log(LogLevel::TRACE, fmt, std::forward<Args>(args)...);
  }

  /// @brief 打印 DEBUG 级日志（开发调试信息）。
  /// @param fmt 格式串，用 {} 作占位符。
  /// @param args 位置参数与 KV(...) 字段。
  template <typename... Args>
  void debug(const char* fmt, Args&&... args) noexcept {
    log(LogLevel::DEBUG, fmt, std::forward<Args>(args)...);
  }

  /// @brief 打印 INFO 级日志（正常业务流程）。
  /// @param fmt 格式串，用 {} 作占位符。
  /// @param args 位置参数与 KV(...) 字段。
  template <typename... Args>
  void info(const char* fmt, Args&&... args) noexcept {
    log(LogLevel::INFO, fmt, std::forward<Args>(args)...);
  }

  /// @brief 打印 WARN 级日志（可恢复异常或潜在风险）。
  /// @param fmt 格式串，用 {} 作占位符。
  /// @param args 位置参数与 KV(...) 字段。
  template <typename... Args>
  void warn(const char* fmt, Args&&... args) noexcept {
    log(LogLevel::WARN, fmt, std::forward<Args>(args)...);
  }

  /// @brief 打印 ERROR 级日志（当前操作失败）。
  /// @param fmt 格式串，用 {} 作占位符。
  /// @param args 位置参数与 KV(...) 字段。
  template <typename... Args>
  void error(const char* fmt, Args&&... args) noexcept {
    log(LogLevel::ERROR, fmt, std::forward<Args>(args)...);
  }

  /// @brief 打印 FATAL 级日志（致命错误）。按 StackTraceMode 默认会自动附上调用栈。
  /// @param fmt 格式串，用 {} 作占位符。
  /// @param args 位置参数与 KV(...) 字段。
  template <typename... Args>
  void fatal(const char* fmt, Args&&... args) noexcept {
    log(LogLevel::FATAL, fmt, std::forward<Args>(args)...);
  }

  /// @brief 记录异常，自动展开为 error / error_type / error_chain 字段。
  ///
  /// `error` 取最内层异常的 what()（即根因），`error_type` 取可读类型名，
  /// `error_chain` 仅在存在嵌套时输出（完整 caused-by 链）。额外 KV 字段照常附加；
  /// 需要调用栈时显式传 `KV("stacktrace", StackTrace::capture())`。
  ///
  /// @param level 日志级别。
  /// @param file 源文件名。
  /// @param line 源文件行号。
  /// @param func 所在函数名。
  /// @param msg 说明文字。
  /// @param e 异常对象。
  /// @param args 额外 KV(...) 字段。
  /// @code
  /// LOG_EXCEPTION("db query failed", e, KV("retry", 3));
  /// @endcode
  template <typename... Args>
  void log_exception(LogLevel level, const char* file, int line, const char* func, const char* msg,
                     const std::exception& e, Args&&... args) noexcept;

  /// @brief 记录异常（std::exception_ptr 版本），自动展开为 error / error_type /
  ///        error_chain 字段；字段含义见上一个重载。
  ///        用于跨线程传递异常的场景——异常对象无法直接传递，只能传 exception_ptr。
  /// @param level 日志级别。
  /// @param file 源文件名。
  /// @param line 源文件行号。
  /// @param func 所在函数名。
  /// @param msg 说明文字。
  /// @param e 异常指针，空指针会被记为占位内容而不是崩溃。
  /// @param args 额外 KV(...) 字段。
  template <typename... Args>
  void log_exception(LogLevel level, const char* file, int line, const char* func, const char* msg,
                     const std::exception_ptr& e, Args&&... args) noexcept;

  /// @brief 记录异常的结构化入口（不带文件 / 行号 / 函数名），级别固定为 ERROR。
  ///        自动展开 error / error_type / error_chain 字段。
  /// @param msg 说明文字。
  /// @param e 异常对象。
  void exception(const char* msg, const std::exception& e) noexcept {
    log_exception(LogLevel::ERROR, nullptr, 0, nullptr, msg, e);
  }

  /// @brief 记录异常的结构化入口（不带文件 / 行号 / 函数名），级别固定为 ERROR。
  ///        接受 std::exception_ptr，适用于跨线程传递异常的场景。
  /// @param msg 说明文字。
  /// @param e 异常指针，空指针会被记为占位内容而不是崩溃。
  void exception(const char* msg, const std::exception_ptr& e) noexcept {
    log_exception(LogLevel::ERROR, nullptr, 0, nullptr, msg, e);
  }

  /// @brief 增加一个输出目标。可挂多个，同一条日志会依次写入每一个。
  /// @param log_sink 输出槽；自定义 Sink 继承 LogSink 即可接入任意后端。
  void add_sink(std::shared_ptr<LogSink> log_sink);

  /// @brief 整份替换运行期配置（线程安全）。
  /// @param config 新配置。未显式赋值的项会恢复默认值，而不是保留旧值。
  /// @note 首次传入 async=true 会惰性启动后台线程；此后无法关闭
  ///       （后续传入 async=false 会被忽略，避免出现"线程还在但不再消费队列"的状态）。
  void set_config(const LogConfig& config);

  /// @brief 只调整日志级别（线程安全），其余配置保持不变，立即对热路径生效。
  /// @param level 新级别。
  /// @note 只对**新记录**生效：已构造或已入队的记录持有各自的配置快照。
  ///       生产上通常由管理接口（HTTP / SIGHUP / 配置中心）触发；
  ///       持久化与鉴权属于应用职责，本库不涉及。
  void set_level(LogLevel level);

  /// @brief 获取当前配置的一份快照。
  /// @return 配置副本（纯值结构，可安全跨线程持有与修改）。
  LogConfig get_config();

  /// @brief 刷新：等待异步队列清空并写完，然后刷新所有 Sink 的缓冲。
  /// @note 同时会补发当前线程挂起的聚合去重摘要。其它线程的挂起摘要要等它们
  ///       各自的下一条不同日志才会补发（聚合状态是 thread_local，无法跨线程触及）。
  void flush_all();

  /// @brief 关闭日志器：停止接收、等队列消费完、刷新 Sink，并回收后台线程。
  /// @return 关闭时的统计快照。
  /// @note 可从多线程并发调用，只有第一个调用真正执行关闭。关闭后不可重开，
  ///       之后再打日志会被丢弃并计入 LogStats::dropped。
  ///       单例的析构函数会自动调用本函数，通常无需显式调用。
  LogStats close();

  /// @brief 获取日志器自身的统计快照。
  /// @return LogStats 副本。计数走原子读，不加锁；仅 queue_length 需短暂持有队列锁。
  [[nodiscard]] LogStats stats();

  /// @brief 预绑定字段，得到一个带固定字段的子 Logger。
  /// @param args 一个或多个 KV(...) 字段。
  /// @return 子 Logger：与父共享 Sink / 配置 / 锁，额外字段会附加到它打出的每条日志，
  ///         并继承父已绑定的字段（可链式叠加）。
  /// @note 子 Logger 的字段优先级低于 ContextScope，更低于调用点的显式 KV。
  /// @code
  /// auto db_logger = Logger::get_instance().with(KV("service", "db"));
  /// db_logger.info("connected");
  /// @endcode
  template <typename... Args>
  Logger with(Args&&... args);

  /// @brief 预绑定 `request_id` 字段，等价于 `with(KV("request_id", id))`。
  /// @param id 请求标识。
  /// @return 带该字段的子 Logger。
  Logger with_request_id(std::string_view id);

  /// @brief 预绑定 `trace_id` 字段，等价于 `with(KV("trace_id", id))`。
  /// @param id 32 位小写 hex。
  /// @return 带该字段的子 Logger。
  Logger with_trace_id(std::string_view id);

  /// @brief 预绑定 `span_id` 字段，等价于 `with(KV("span_id", id))`。
  /// @param id 16 位小写 hex。
  /// @return 带该字段的子 Logger。
  Logger with_span_id(std::string_view id);

  /// @brief 预绑定 `user_id` 字段，等价于 `with(KV("user_id", id))`。
  /// @param id 用户标识。
  /// @return 带该字段的子 Logger。
  Logger with_user_id(std::string_view id);

  /// @brief 预绑定 `service` 字段，等价于 `with(KV("service", name))`。
  /// @param name 服务名。
  /// @return 带该字段的子 Logger。
  Logger with_service(std::string_view name);

  /// @brief 一次绑定 `trace_id` / `span_id` / `trace_flags` 三个字段。
  /// @param ctx 链路上下文，通常来自 parse_traceparent() 或 generate_trace()。
  /// @return 带这三个字段的子 Logger。
  Logger with_trace(const TraceContext& ctx);

  /// @brief 析构：仅当本对象持有 impl_ 的所有权（即单例）时停止后台线程并刷盘
  ///        （等价于调用 close()）。`with()` 得到的子 Logger 共享同一份状态但不持有
  ///        所有权，析构不做任何事 —— 否则一个临时子对象离开作用域就会关掉整个日志器。
  ~Logger();

 private:
  // 构造函数全部私有：外部只能拿到单例引用，或经 with() 得到一个共享状态的副本。

  /// @brief 默认构造：只有单例走这条路，因此只有它持有 impl_ 的所有权。
  Logger() = default;

  /// @brief 拷贝构造：与源共享同一份状态（Sink / 配置 / 后台线程），但不持有所有权。
  /// @param o 源对象。
  Logger(const Logger& o) : impl_(o.impl_), fields_(o.fields_), owner_(false) {}

  /// @brief 拷贝赋值已禁用：状态是共享的，赋值会让两个对象指向同一份状态却各持一份字段。
  Logger& operator=(const Logger&) = delete;

  /// @brief 移动赋值已禁用。移动构造未声明（有拷贝构造与析构，编译器不会隐式生成），
  ///        因此移动按拷贝处理。
  Logger& operator=(Logger&&) = delete;

  /// @brief 队列元素：待写出的记录及其"是否省略调试信息"标记。
  struct LogData {
    Record msg;  ///< 已物化的记录
    bool less;   ///< true 表示结构化入口，格式化时不输出文件 / 行号 / 函数
  };

  /// @brief 自身指标的原子累加器。热路径只做 relaxed 增减，stats() 无锁快照。
  struct Counters {
    std::atomic<unsigned long long> written{0};           ///< 成功写入 sink 的记录数
    std::atomic<unsigned long long> failed_writes{0};     ///< sink 写失败次数
    std::atomic<unsigned long long> dropped{0};           ///< 丢弃次数（各类原因合计）
    std::atomic<unsigned long long> dedup_suppressed{0};  ///< 被聚合去重抑制的条数
    std::atomic<unsigned long long> masked_fields{0};     ///< 脱敏字段数
    std::atomic<unsigned long long> by_level[7] = {};  ///< 按级别统计（下标同 LogLevel）
    std::atomic<unsigned long long> queue_peak{0};     ///< 队列长度峰值
    std::atomic<unsigned long long> max_write_latency_us{0};  ///< 异步写出最长延迟（微秒）
  };

  /// @brief 单例共享的全部可变状态。子 Logger（with() 的产物）共享同一个 Impl。
  struct Impl {
    std::mutex mtx;                               ///< 保护 sinks / config / async_running
    std::vector<std::shared_ptr<LogSink>> sinks;  ///< 输出目标
    LogConfig config;                             ///< 配置快照源
    /// 级别热缓存：热路径靠它无锁早退，必须与 config.log_level 同步更新
    std::atomic<LogLevel> log_level{LogLevel::TRACE};
    std::atomic<bool> async_running{false};  ///< 后台线程是否已启动（启动后不变）
    Counters counters;                       ///< 自身指标

    std::mutex queue_mtx;            ///< 保护 queue / stop / in_flight
    std::condition_variable cv;      ///< 队列非空或收到停止信号时唤醒后台线程
    std::condition_variable ful_cv;  ///< 队列腾出空间时唤醒被阻塞的生产者
    std::deque<LogData> queue;       ///< 异步队列，仅异步模式使用
    std::atomic<bool> stop{false};  ///< 是否已关闭：close() 置位，此后不再接收新日志
    std::atomic<bool> in_flight{false};  ///< 后台线程是否正在写一条，供 flush_all 判定

    std::thread async_thread;   ///< 后台写入线程
    std::once_flag close_once;  ///< 保证并发 close() 只有一个线程真正停止并 join
  };

  /// @brief 后台线程主循环：取队列 → 写出 → 复位 in_flight；每条都用 try 兜底，
  ///        避免异常穿出线程函数导致进程终止或 flush_all 永久等待。
  void async_loop();

  /// @brief 后台线程写一条：快照 config/sinks，格式化后写各 Sink 并更新指标。
  /// @param data 待写出的记录。
  void write_one(const LogData& data);

  /// @brief DropDebug 策略：按级别从低到高，从队尾删掉最先找到的一条，再压入新记录。
  /// @param data 要压入的新记录。
  void drop_debug(LogData&& data);

  /// @brief 同步分支：格式化后加窄锁写 Sink（Sink 本身非线程安全）。
  /// @param msg 已物化的记录。
  /// @param config 配置快照。
  /// @param sinks 输出目标快照。
  /// @param less true 表示不输出文件 / 行号 / 函数。
  void log_impl_sync(const Record& msg, const LogConfig& config,
                     const std::vector<std::shared_ptr<LogSink>>& sinks, bool less);

  /// @brief 日志入口：级别无锁早退 + 全库唯一的异常兜底，随后交给 log_impl_inner。
  /// @param logLevel 日志级别。
  /// @param file 源文件名。
  /// @param line 源文件行号。
  /// @param func 所在函数名。
  /// @param fmt 格式串，用 {} 作占位符。
  /// @param args 位置参数与 KV(...) 字段。
  template <typename... Args>
  void log_impl(LogLevel logLevel, const char* file, int line, const char* func, const char* fmt,
                Args&&... args) noexcept;

  /// @brief log_impl 的实际实现，允许抛异常，由 log_impl 统一兜底。
  /// @param logLevel 日志级别。
  /// @param file 源文件名。
  /// @param line 源文件行号。
  /// @param func 所在函数名。
  /// @param fmt 格式串，用 {} 作占位符。
  /// @param args 位置参数与 KV(...) 字段。
  template <typename... Args>
  void log_impl_inner(LogLevel logLevel, const char* file, int line, const char* func,
                      const char* fmt, Args&&... args);

  /// @brief 把异常展开成字段后复用 log_impl。无嵌套时 error_chain 用空 key 占位，
  ///        由 append_field 跳过。
  /// @param level 日志级别。
  /// @param file 源文件名。
  /// @param line 源文件行号。
  /// @param func 所在函数名。
  /// @param msg 说明文字。
  /// @param info 已提取的异常信息。
  /// @param args 额外 KV(...) 字段。
  template <typename... Args>
  void log_exception_impl(LogLevel level, const char* file, int line, const char* func,
                          const char* msg, const detail::ExceptionInfo& info,
                          Args&&... args) noexcept;

  /// @brief 按配置的格式选择对应 Formatter 并格式化。
  /// @param msg 记录。
  /// @param config 配置。
  /// @param less true 表示省略调试信息。
  /// @return 交给 Sink 的成品。
  SinkInput format_record(const Record& msg, const LogConfig& config, bool less);

  /// @brief 按 max_record_size 削减后格式化。
  ///
  /// 削减顺序：先丢末尾字段（保住消息正文），字段丢完仍超则按超出量截断消息。
  /// 最少形态是「框架 + "..."」——框架（时间戳 / 级别 / 文件行号）必输出，
  /// 切了整条记录就无法解析。绝不对格式化结果做字节级硬切。
  ///
  /// @param msg 记录。
  /// @param config 配置。
  /// @param less true 表示省略调试信息。
  /// @return 格式化结果，长度受 max_record_size 约束。
  SinkInput format_record_budgeted(const Record& msg, const LogConfig& config, bool less);

  /// @brief 敏感字段脱敏：按 key 调用脱敏函数，只有值真被改变才重建 FieldValue，
  ///        未命中者保持原类型（否则数字字段会退化成字符串）。
  /// @param fields 待处理字段。
  /// @param config 配置。
  /// @note 脱敏函数抛出的异常向上抛，由 log_impl 丢弃整条 —— 绝不"保留原值继续输出"，
  ///       那等于把敏感数据原样写进日志。
  void mask_sensitive_fields(std::vector<Field>& fields, const LogConfig& config);

  /// @brief 按 max_field_length 阶段超长字段值：UTF-8 安全截断并加 "..."。
  ///        度量按目标格式渲染后进行 —— 同一个值在 text 与 JSON 下长度不同。
  /// @param fields 待处理字段。
  /// @param config 配置。
  void limit_field_lengths(std::vector<Field>& fields, const LogConfig& config);

  /// @brief 补发聚合去重摘要：取走载荷，填入重复次数后输出。
  /// @param count 该序列的总次数（调用方保证 > 1）。
  /// @param config 配置。
  /// @param is_async 是否异步模式。
  /// @param sinks 输出目标快照。
  void emit_dedup_summary(std::uint64_t count, const LogConfig& config, bool is_async,
                          const std::vector<std::shared_ptr<LogSink>>& sinks);

  /// @brief 当前线程待补发的聚合载荷：序列首次重复时组装（含上下文 / 堆栈 / 字段），
  ///        序列结束时输出。
  /// @return 载荷引用（thread_local）。
  static Record& pending_dedup_payload();

  /// @brief 载荷是否已成功组装。组装中途失败时保持 false，避免发出空记录。
  /// @return 标志引用（thread_local）。
  static bool& pending_dedup_ready();

  /// @brief 结束当前线程的去重序列并补发摘要，供 flush_all 调用。
  void flush_dedup() noexcept;

  /// @brief 在队列满队判断处更新峰值，避免在每个 push/pop 点各记一次。
  ///        满队时后续分支大小不变（n），未满时压入后为 n+1。
  /// @param n 本次观测到的队列长度。
  void note_queue_peak(std::size_t n) noexcept {
    if (n > impl_->counters.queue_peak.load(std::memory_order_relaxed))
      impl_->counters.queue_peak.store(n, std::memory_order_relaxed);
  }

  /// @brief 路由一条已物化的记录：异步入队 / 同步直写。
  /// @param msg 记录。
  /// @param config 配置快照。
  /// @param is_async 是否异步模式。
  /// @param sinks 输出目标快照；异步路径下为空，由后台线程自行快照。
  /// @note 刻意不标 noexcept —— 同步分支的格式化会抛，需向上交给 log_impl 兜底。
  void route_record(Record&& msg, const LogConfig& config, bool is_async,
                    const std::vector<std::shared_ptr<LogSink>>& sinks);

  std::shared_ptr<Impl> impl_ = std::make_shared<Impl>();  ///< 共享状态
  std::vector<Field> fields_;                              ///< 预绑定字段，with() 累积
  bool owner_ = true;  ///< 是否持有 impl_ 的所有权：只有单例为 true，其析构才关闭日志器
};

/// @brief 取路径中的文件名部分；constexpr，在编译期求值。
/// @param path 完整路径（__FILE__）。
/// @return 指向文件名起始处的指针；无路径分隔符时返回原指针。
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

/// @name 日志宏
/// 统一使用 LOG_ 前缀，避免与业务宏冲突。宏入口自动携带文件名 / 行号 / 函数名，
/// 并在命中级别过滤时于入口处早退。
/// @{

/// @brief 打印 TRACE 级日志。
/// @param fmt 格式串，用 {} 作占位符。
/// @param ... 位置参数与 KV(...) 字段。
#define LOG_TRACE(fmt, ...)                                                                        \
  ::logger::Logger::get_instance().log(::logger::LogLevel::TRACE, ::logger::filename_of(__FILE__), \
                                       __LINE__, __func__, fmt, ##__VA_ARGS__)

/// @brief 打印 DEBUG 级日志。
/// @param fmt 格式串，用 {} 作占位符。
/// @param ... 位置参数与 KV(...) 字段。
#define LOG_DEBUG(fmt, ...)                                                                        \
  ::logger::Logger::get_instance().log(::logger::LogLevel::DEBUG, ::logger::filename_of(__FILE__), \
                                       __LINE__, __func__, fmt, ##__VA_ARGS__)

/// @brief 打印 INFO 级日志。
/// @param fmt 格式串，用 {} 作占位符。
/// @param ... 位置参数与 KV(...) 字段。
#define LOG_INFO(fmt, ...)                                                                        \
  ::logger::Logger::get_instance().log(::logger::LogLevel::INFO, ::logger::filename_of(__FILE__), \
                                       __LINE__, __func__, fmt, ##__VA_ARGS__)

/// @brief 打印 WARN 级日志。
/// @param fmt 格式串，用 {} 作占位符。
/// @param ... 位置参数与 KV(...) 字段。
#define LOG_WARN(fmt, ...)                                                                        \
  ::logger::Logger::get_instance().log(::logger::LogLevel::WARN, ::logger::filename_of(__FILE__), \
                                       __LINE__, __func__, fmt, ##__VA_ARGS__)

/// @brief 打印 ERROR 级日志。
/// @param fmt 格式串，用 {} 作占位符。
/// @param ... 位置参数与 KV(...) 字段。
#define LOG_ERROR(fmt, ...)                                                                        \
  ::logger::Logger::get_instance().log(::logger::LogLevel::ERROR, ::logger::filename_of(__FILE__), \
                                       __LINE__, __func__, fmt, ##__VA_ARGS__)

/// @brief 打印 FATAL 级日志；按 StackTraceMode 默认会自动附上调用栈。
/// @param fmt 格式串，用 {} 作占位符。
/// @param ... 位置参数与 KV(...) 字段。
#define LOG_FATAL(fmt, ...)                                                                        \
  ::logger::Logger::get_instance().log(::logger::LogLevel::FATAL, ::logger::filename_of(__FILE__), \
                                       __LINE__, __func__, fmt, ##__VA_ARGS__)

/// @brief 记录异常并自动展开字段（error / error_type / error_chain），级别为 ERROR。
/// @param msg 说明文字。
/// @param exc 异常对象或 std::exception_ptr。
/// @param ... 额外 KV(...) 字段。
#define LOG_EXCEPTION(msg, exc, ...)                                                        \
  ::logger::Logger::get_instance().log_exception(::logger::LogLevel::ERROR,                 \
                                                 ::logger::filename_of(__FILE__), __LINE__, \
                                                 __func__, (msg), (exc), ##__VA_ARGS__)
/// @}

template <typename... Args>
Logger Logger::with(Args&&... args) {
  static_assert((std::is_same_v<std::decay_t<Args>, Field> && ...), "with() 仅接受 KV(...) 字段");
  std::unique_lock<std::mutex> lock(impl_->mtx);
  Logger child;
  child.impl_ = impl_;      // 与父共享 Sink / 配置 / 锁
  child.fields_ = fields_;  // 继承父已绑定的字段
  child.owner_ = false;     // 状态是共享的，子 Logger 不负责关闭它
  (append_field(child.fields_, std::forward<Args>(args)), ...);
  return child;  // 拷贝构造：新副本同样不持有所有权
}

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

template <typename... Args>
void Logger::log_impl(LogLevel logLevel, const char* file, int line, const char* func,
                      const char* fmt, Args&&... args) noexcept {
  // 级别过滤（无锁早退）：关闭的级别不付任何后续开销
  if (logLevel < impl_->log_level.load(std::memory_order_relaxed))
    return;

  // 已关闭：丢弃并计数。close() 之后队列不再有消费者，放进去只会占内存；
  // Block 策略下还会等一个永远不会到来的空间
  if (impl_->stop.load(std::memory_order_relaxed)) {
    impl_->counters.dropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  try {
    log_impl_inner(logLevel, file, line, func, fmt, std::forward<Args>(args)...);
  } catch (...) {
    // 任何异常（内存不足、用户 operator<< / 脱敏函数抛出、库自身的意外）都整条丢弃：
    // 日志故障不能影响主业务，也不能让进程倒下
    impl_->counters.dropped.fetch_add(1, std::memory_order_relaxed);
  }
}

template <typename... Args>
// 两个相邻的 const char*（func / fmt）确实可能写反；但调用点只有 LOG_* 宏，位置固定且不对外开放
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void Logger::log_impl_inner(LogLevel logLevel, const char* file, int line, const char* func,
                            const char* fmt, Args&&... args) {
  // 一次持锁快照：字段 / 配置 / 输出目标（异步路径下 sinks 留给后台线程快照）
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
      sinks = impl_->sinks;
    stacktrace = config.stacktrace == StackTraceMode::ALWAYS ||
                 (config.stacktrace == StackTraceMode::FATAL && logLevel >= LogLevel::FATAL);
  }

  // 聚合去重：同 (level, file, line) 在窗口内只输出首条，序列结束补一条重复次数。
  // 判定必须早于合并上下文 / 采集堆栈 / 构造 Record —— 重复日志这些开销全都要省掉
  bool capture_only = false;
  if (config.dedup_window_ms != 0) {
    std::uint64_t prev_count = 0;
    bool need_payload = false;
    const auto decision = dedup_filter().on_log(logLevel, file, line, config.dedup_window_ms,
                                                &prev_count, &need_payload);
    if (prev_count > 1)
      emit_dedup_summary(prev_count, config, is_async, sinks);

    if (decision == DedupFilter::Decision::Suppress) {
      impl_->counters.dedup_suppressed.fetch_add(1, std::memory_order_relaxed);
      if (!need_payload)
        return;  // 窗口内重复：不构造 Record、不入队、不落盘
      // 首次重复：照常组装完整 Record（含上下文 / 堆栈 / 字段），但只存不发
      capture_only = true;
    }
  }

  // 合并 thread_local 上下文栈里的字段（with() 的字段已在上面快照进来）
  ContextScope::merge_into(fields);

  // Fatal 自动采集堆栈；显式 KV("stacktrace", ...) 由调用方自行附加
  if (stacktrace)
    append_field(fields, KV("stacktrace", StackTrace::capture(1, config.stacktrace_depth,
                                                              config.max_stacktrace_length)));

  // 物化 Record（无锁，本地工作）：位置参数进 tuple，KV 字段并入 fields
  auto positional = split_fields(fields, std::forward<Args>(args)...);
  dedup_fields(fields);  // 同 key 后写覆盖，保留首次出现的位置

  // 脱敏必须早于构造 Record：异步模式下队列里不该存明文
  mask_sensitive_fields(fields, config);
  // 位置参数经用户 operator<< 编码，可能抛；统一由 log_impl 兜底
  std::string content =
      std::apply([&](auto&&... pa) { return detail::format(fmt, pa...); }, positional);

  // 部件上限：先限制各部件，整条预算由 format_record_budgeted 兜底
  limit_field_lengths(fields, config);
  if (config.max_message_length != 0)
    truncate_utf8(content, config.max_message_length);

  Record msg{std::chrono::system_clock::now(),
             logLevel,
             std::move(content),
             std::this_thread::get_id(),
             file,
             line,
             func,
             std::move(fields)};

  // 首次重复：把组装好的 Record 留作摘要载荷，不立即输出
  if (capture_only) {
    pending_dedup_payload() = std::move(msg);
    pending_dedup_ready() = true;
    return;
  }

  route_record(std::move(msg), config, is_async, sinks);
}

inline void Logger::route_record(Record&& msg, const LogConfig& config, bool is_async,
                                 const std::vector<std::shared_ptr<LogSink>>& sinks) {
  const bool less = (msg.file == nullptr);

  if (!is_async) {
    log_impl_sync(msg, config, sinks, less);
    return;
  }

  bool full;
  {
    std::unique_lock<std::mutex> qlock(impl_->queue_mtx);
    const std::size_t n = impl_->queue.size();
    full = n >= config.buffer_size;
    // 峰值就地更新：队列满时后续分支大小不变（n），未满时压入后为 n+1
    note_queue_peak(full ? n : n + 1);
  }
  if (full) {
    switch (config.asy_que_ful_strategy) {
    case AsyQueFulStrategy::Block: {
      // 等后台线程腾出空间再压入，尽量不丢
      std::unique_lock<std::mutex> qlock(impl_->queue_mtx);
      impl_->ful_cv.wait(qlock, [this, &config]() {
        return impl_->queue.size() < config.buffer_size ||
               impl_->stop.load(std::memory_order_relaxed);
      });
      if (impl_->stop.load(std::memory_order_relaxed)) {  // 等待期间被关闭：丢弃
        impl_->counters.dropped.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      impl_->queue.push_back(LogData{std::move(msg), less});
      break;
    }
    case AsyQueFulStrategy::DropNewest: {
      impl_->counters.dropped.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    case AsyQueFulStrategy::DropOldest: {
      std::unique_lock<std::mutex> qlock(impl_->queue_mtx);
      impl_->queue.pop_front();
      impl_->queue.push_back(LogData{std::move(msg), less});
      impl_->counters.dropped.fetch_add(1, std::memory_order_relaxed);
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
}

template <typename... Args>
void Logger::log_exception_impl(LogLevel level, const char* file, int line, const char* func,
                                const char* msg, const detail::ExceptionInfo& info,
                                Args&&... args) noexcept {
  // error_chain 只在有嵌套时产生；无嵌套用空 key 占位，append_field 会跳过它
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

}  // namespace logger
