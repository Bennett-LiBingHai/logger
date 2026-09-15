#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace logger {
/// @file level.h
/// @brief 日志级别枚举及其文本转换。

/// @brief 日志级别，取值由低到高递增。
///
/// 级别之间是**包含关系**：设置级别为 INFO 时，INFO 及更高的 WARN / ERROR / FATAL
/// 都会输出，比它低的则被丢弃。Logger 在入口处做这个比较，关闭的级别不付任何后续开销。
enum class LogLevel : std::uint8_t {
  TRACE = 0,  ///< 极细粒度的执行过程
  DEBUG,      ///< 开发调试信息
  INFO,       ///< 正常业务流程
  WARN,       ///< 可恢复异常或潜在风险
  ERROR,      ///< 当前操作失败
  FATAL,      ///< 致命错误，记录后通常调用 std::abort() 终止进程
  OFF         ///< 关闭所有日志输出
};

/// @brief 取级别的大写名字，用于文本格式输出。
/// @param log_level 日志级别。
/// @return 形如 "INFO"、"ERROR" 的常量字符串；取值非法时返回 "UNKNOWN"。
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

/// @brief 取级别的小写名字，用于 JSON 格式输出（JSON 惯例是小写）。
/// @param log_level 日志级别。
/// @return 形如 "info"、"error" 的字符串；取值非法时返回 "unknown"。
[[nodiscard]] inline std::string to_lower_string(LogLevel log_level) {
  switch (log_level) {
  case LogLevel::TRACE:
    return "trace";
  case LogLevel::DEBUG:
    return "debug";
  case LogLevel::INFO:
    return "info";
  case LogLevel::WARN:
    return "warn";
  case LogLevel::ERROR:
    return "error";
  case LogLevel::FATAL:
    return "fatal";
  case LogLevel::OFF:
    return "off";
  default:
    return "unknown";
  }
}

}  // namespace logger
