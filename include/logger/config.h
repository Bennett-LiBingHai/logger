#pragma once
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

// 日志自身统计（M3 起步，M6 扩展）
struct LogStats {
  unsigned long long failed_writes = 0;  // 写失败次数
  unsigned long long dropped = 0;        // 丢弃次数
};

// 日志器配置（纯值结构，可拷贝/快照；热路径要 lock-free 读的级别单独放 Impl 里的原子）
struct LogConfig {
  LogLevel log_level = LogLevel::TRACE;          // 默认日志级别,低于该级别会忽略
  size_t max_log_item_size = 1024;               // 最大的一条日志长度
  TimeFormat time_format = TimeFormat::ISO8601;  // 日期格式
  bool use_utc_time = false;                     // 是否使用0时区时间,否则本地时间
  LogFormat format = LogFormat::TEXT;            // 输出格式：文本 / JSON
  LogFailStrategy log_fail_strategy = LogFailStrategy::FallbackToStderr;  // 日志输出失败策略
  StackTraceMode stacktrace = StackTraceMode::FATAL;  // 堆栈采集策略,只约束自动采集
  size_t stacktrace_depth = 10;                       // 单个堆栈最大帧数
  size_t max_stacktrace_length = 512;                 // 堆栈渲染后字节上限（0 = 不限）

  // 异步相关
  bool async = false;  // 异步日志（set_config 时惰性启动后台线程）
  unsigned long long buffer_size = 10000;  // 异步日志队列最大空间
  AsyQueFulStrategy asy_que_ful_strategy = AsyQueFulStrategy::Block;
};
