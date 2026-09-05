#pragma once
#include <string_view>

// 日志级别
enum class LogLevel {
  TRACE = 0,  // 极细粒度的执行过程
  DEBUG,      // 开发调试信息
  INFO,       // 正常业务流程
  WARN,       // 可恢复异常或潜在风险
  ERROR,      // 当前操作失败
  FATAL,      // 致命错误，记录后通常调用 `std::abort()` 终止进程
  OFF         // 关闭所有日志输出
};

// 获取日志级别对应字符串
[[nodiscard]] inline const std::string_view to_string(LogLevel log_level) {
  switch (log_level) {
  case LogLevel::TRACE:
    return "TRACE";
  case LogLevel::DEBUG:
    return "DEBUG";
  case LogLevel::INFO:
    return "INFO";
  case LogLevel::WARN:
    return "WARN";
  case LogLevel::ERROR:
    return "ERROR";
  case LogLevel::FATAL:
    return "FATAL";
  case LogLevel::OFF:
    return "OFF";
  default:
    return "UNKNOWN";
  }
}
