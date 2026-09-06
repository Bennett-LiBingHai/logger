#pragma once
#include "logger/level.h"
#include "logger/time_format.h"

// 输出格式
enum class LogFormat {
  TEXT = 0,  // 文本格式
  JSON,      // JSON 格式
};

// 日志器配置
struct LogConfig {
  LogLevel log_level = LogLevel::TRACE;          // 默认日志级别,,低于该级别会忽略
  size_t max_log_item_size = 1024;               // 最大的一条日志长度
  TimeFormat time_format = TimeFormat::ISO8601;  // 日期格式
  bool use_utc_time = false;                     // 是否使用0时区时间,否则本地时间
  LogFormat format = LogFormat::TEXT;            // 输出格式：文本 / JSON
};
