#pragma once
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "logger/level.h"

/// @file config.h
/// @brief 运行期配置、统计快照与敏感字段判定。

/// @brief 输出格式。
enum class LogFormat : std::uint8_t {
  TEXT = 0,  ///< 文本：一条一行，形如 `时间[级别][位置]正文 字段...`
  JSON,      ///< JSON：一条一行，便于下游按字段检索
};

/// @brief 写 Sink 失败时的策略。
enum class LogFailStrategy : std::uint8_t {
  FallbackToStderr = 0,  ///< 降级：把这条日志写到 stderr，尽量不丢
  Drop,                  ///< 丢弃：直接放弃这条日志
};

/// @brief 时间格式。
enum class TimeFormat : std::uint8_t {
  /// ISO8601，形如 `YYYY-MM-DDTHH:mm:ss.fff`；用本地时间还是 UTC 由 use_utc_time 决定
  ISO8601 = 0,
};

/// @brief 自动采集调用栈的时机。
enum class StackTraceMode : std::uint8_t {
  OFF = 0,  ///< 完全关闭，显式请求也不生效（生产排障时可用作全局开关）
  FATAL,  ///< 仅 FATAL 自动采集（默认）；普通 ERROR 不采，避免采集与符号化开销
  ALWAYS,  ///< 所有日志都采集（调试用，开销大）
};

/// @brief 异步队列满时的处理策略。
enum class AsyQueFulStrategy : std::uint8_t {
  Block = 0,   ///< 阻塞调用方直到有空间，尽量不丢日志
  DropNewest,  ///< 丢弃新来的这条
  DropOldest,  ///< 丢弃队列里最旧的一条
  DropDebug,   ///< 按级别从低到高找一条丢弃，优先牺牲低级别日志
};

/// @brief 日志器自身的统计快照。
///
/// 内部用原子累加，stats() 读取时不需要加锁（仅 queue_length 需短暂持队列锁）。
/// 取差分时注意：`written` 按「记录」计，`failed_writes` 与 `dropped` 按「次」计。
struct LogStats {
  unsigned long long written = 0;  ///< 成功写入 Sink 的记录数（至少一个 Sink 成功）
  unsigned long long failed_writes = 0;  ///< Sink 写失败次数
  unsigned long long dropped = 0;  ///< 丢弃次数：队列满、写失败按策略丢弃、编码异常
  unsigned long long dedup_suppressed = 0;  ///< 被聚合去重抑制的条数
  unsigned long long masked_fields = 0;     ///< 被脱敏的字段数
  unsigned long long by_level[7] = {};      ///< 按级别统计写入条数，下标同 LogLevel
  unsigned long long queue_length = 0;  ///< 快照时刻的队列长度（同步模式恒为 0）
  unsigned long long queue_peak = 0;    ///< 队列长度峰值
  unsigned long long max_write_latency_us = 0;  ///< 异步写出最长延迟（微秒）
};

/// @brief 判断字段名是否命中默认敏感关键词表。
///
/// 单独暴露出来，是为了让自定义脱敏函数能复用它，不必把关键词表重抄一遍。
/// 匹配方式是**精确相等**，不是子串包含。
///
/// @param key 字段名。
/// @return true 表示该字段名属于默认敏感集合。
/// @code
/// cfg.sensitive_field_masker = [](const std::string& key, const std::string& v) {
///   if (!is_sensitive_key(key)) return v;
///   return v.substr(0, 6) + "****" + v.substr(v.size() - 4);  // 卡号保留前 6 后 4
/// };
/// @endcode
inline bool is_sensitive_key(std::string_view key) {
  static constexpr std::string_view kKeys[] = {
      "password",      "passwd", "token",       "access_token", "refresh_token", "secret",
      "authorization", "cookie", "private_key", "credit_card",  "id_card"};
  for (std::string_view k : kKeys) {
    if (key == k)
      return true;
  }
  return false;
}

/// @brief 默认脱敏函数：命中敏感字段名则整体隐藏为 `***`，否则原样返回。
/// @param key 字段名。
/// @param value 字段值的**未编码原始值**（不含引号、未转义）。
/// @return 脱敏后的值；未命中时原样返回。
/// @note 定义在 LogConfig 之前 —— 它被用作成员默认值。
inline std::string default_sensitive_field_masker(const std::string& key,
                                                  const std::string& value) {
  return is_sensitive_key(key) ? std::string("***") : value;
}

/// @brief 日志器配置（纯值结构，可整体拷贝 / 快照）。
///
/// 热路径需要无锁读取的日志级别单独存放在 Logger 内部的原子里；本结构中的
/// log_level 只在 set_config / set_level 时被读取。
struct LogConfig {
  /// @brief 脱敏函数签名：入参与返回都是**未编码的原始值**，因此无需区分 text 与 JSON。
  using MaskerFn = std::function<std::string(const std::string& key, const std::string& value)>;

  LogLevel log_level = LogLevel::TRACE;  ///< 最低输出级别，低于它的日志在入口处被丢弃
  size_t max_message_length = 0;         ///< 消息正文的字节上限，0 = 不限
  size_t max_field_length = 0;           ///< 单个字段值的字节上限，0 = 不限

  /// 整条记录的字节预算，0 = 不限。
  ///
  /// 框架（时间戳 + 级别 + 文件行号）与消息的 `...` 标记必定输出，因此该值小于
  /// 「框架 + ...」时不可满足，记录会以最少形态（框架 + `...`）输出。
  size_t max_record_size = 1024;

  TimeFormat time_format = TimeFormat::ISO8601;  ///< 时间格式
  bool use_utc_time = false;                     ///< true 用 UTC，false 用本地时间
  LogFormat format = LogFormat::TEXT;            ///< 输出格式：文本 / JSON
  LogFailStrategy log_fail_strategy = LogFailStrategy::FallbackToStderr;  ///< 写失败策略
  StackTraceMode stacktrace = StackTraceMode::FATAL;  ///< 自动采集堆栈的时机
  size_t stacktrace_depth = 10;                       ///< 单个堆栈的最大帧数
  size_t max_stacktrace_length = 512;  ///< 堆栈渲染后的字节上限，0 = 不限

  /// 聚合去重窗口（毫秒），0 = 关闭。
  ///
  /// 同一线程内，相同 (level, file, line) 在窗口内只输出首条，序列结束时补一条
  /// 带重复次数的摘要。
  size_t dedup_window_ms = 0;

  bool enable_sensitive_field_mask = false;  ///< 是否启用敏感字段脱敏
  MaskerFn sensitive_field_masker = default_sensitive_field_masker;  ///< 脱敏函数

  bool async = false;  ///< 异步写入开关；set_config 时惰性启动后台线程，开启后不可回退
  unsigned long long buffer_size = 10000;  ///< 异步队列的最大长度
  AsyQueFulStrategy asy_que_ful_strategy = AsyQueFulStrategy::Block;  ///< 队列满时的策略
};
