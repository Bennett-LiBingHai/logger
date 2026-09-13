#pragma once
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "logger/level.h"
// 输出格式
enum class LogFormat {
  TEXT = 0,  // 文本格式
  JSON,      // JSON 格式
};

// 日志输出失败策略
enum class LogFailStrategy {
  FallbackToStderr = 0,  // stderr输出
  Drop                   // 丢弃
};

// 日期格式
enum class TimeFormat {
  ISO8601 = 0,  // YYYY‑MM‑DDTHH:mm:ss.fff (不带z,是否是本地时间,看config配置)
};

// 堆栈采集策略（M5）
enum class StackTraceMode {
  OFF = 0,  // 完全关闭，含显式 WithStack（生产排障时的全局开关，优先级最高）
  FATAL,  // 仅 Fatal 自动采集（默认；普通 Error 不采，避免采集/符号化开销）
  ALWAYS,  // 所有日志自动采集（调试用，开销大）
};

// 异步日志队列满时的策略
enum class AsyQueFulStrategy {
  Block = 0,   // 阻塞调用方，尽量不丢日志
  DropNewest,  // 丢弃新日志
  DropOldest,  // 丢弃旧日志
  DropDebug,   // 优先丢弃低级别日志
};

// 日志自身统计快照（内部用原子计数，读取不加锁）
struct LogStats {
  unsigned long long written = 0;        // 成功写入 sink 的记录数
  unsigned long long failed_writes = 0;  // sink 写失败次数
  unsigned long long dropped = 0;  // 丢弃次数（队列满、写失败按策略丢弃、编码异常）
  unsigned long long dedup_suppressed = 0;  // 被聚合去重抑制的条数
  unsigned long long masked_fields = 0;     // 脱敏字段数
  unsigned long long by_level[7] = {};      // 按级别统计写入条数（下标同 LogLevel）
  unsigned long long queue_length = 0;      // 快照时刻队列长度
  unsigned long long queue_peak = 0;        // 队列峰值
  unsigned long long max_write_latency_us = 0;  // 异步写出最长延迟（微秒）
};

// 敏感字段名判定：命中默认关键词表即视为敏感。
// 单独暴露出来是为了让自定义脱敏函数能复用它——否则每个自定义 masker
// 都得把下面这份关键词表重抄一遍。
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

// 默认脱敏函数：命中敏感字段名则整体隐藏。
// 必须定义在 LogConfig 之前——它被用作成员默认值。
// 需要"部分保留"等更细的规则时，自定义 masker 里调用 is_sensitive_key 即可：
//   cfg.sensitive_field_masker = [](const std::string& key, const std::string& v) {
//     if (!is_sensitive_key(key)) return v;
//     if (v.size() <= 10) return std::string("***");
//     return v.substr(0, 6) + "****" + v.substr(v.size() - 4);   // 前 6 后 4
//   };
inline std::string default_sensitive_field_masker(const std::string& key,
                                                  const std::string& value) {
  return is_sensitive_key(key) ? std::string("***") : value;
}

// 日志器配置（纯值结构，可拷贝/快照；热路径要 lock-free 读的级别单独放 Impl 里的原子）
struct LogConfig {
  // 脱敏函数签名：入参与返回都是**未编码的原始值**，因此无需区分 text / JSON
  using MaskerFn = std::function<std::string(const std::string& key, const std::string& value)>;

  LogLevel log_level = LogLevel::TRACE;  // 默认日志级别,低于该级别会忽略
  size_t max_message_length = 0;         // 消息正文字节上限，0 = 不限
  size_t max_field_length = 0;           // 单个字段值字节上限，0 = 不限
  // 整条记录字节预算，0 = 不限。
  // 框架（时间戳+级别+文件行号）与消息的 "..." 标记必定输出，故该值小于
  // 「框架 + ...」时不可满足，记录会以最少形态（框架 + "..."）输出
  size_t max_record_size = 1024;
  TimeFormat time_format = TimeFormat::ISO8601;  // 日期格式
  bool use_utc_time = false;                     // 是否使用0时区时间,否则本地时间
  LogFormat format = LogFormat::TEXT;            // 输出格式：文本 / JSON
  LogFailStrategy log_fail_strategy = LogFailStrategy::FallbackToStderr;  // 日志输出失败策略
  StackTraceMode stacktrace = StackTraceMode::FATAL;  // 堆栈采集策略,只约束自动采集
  size_t stacktrace_depth = 10;                       // 单个堆栈最大帧数
  size_t max_stacktrace_length = 512;                 // 堆栈渲染后字节上限（0 = 不限）
  // 聚合去重窗口（毫秒），0 = 关闭。
  // 同一线程内，相同 (level, file, line) 在窗口内只输出首条，序列结束补一条重复次数
  size_t dedup_window_ms = 0;
  bool enable_sensitive_field_mask = false;  // 是否启用敏感字段脱敏
  MaskerFn sensitive_field_masker = default_sensitive_field_masker;  // 脱敏函数,返回脱敏后的值

  // 异步相关
  bool async = false;  // 异步日志（set_config 时惰性启动后台线程）
  unsigned long long buffer_size = 10000;  // 异步日志队列最大空间
  AsyQueFulStrategy asy_que_ful_strategy = AsyQueFulStrategy::Block;
};
