#pragma once
#include <ostream>
#include <string>

#include "logger/level.h"

// 格式化器返回值
struct FormatResult {
  std::string formatted_msg;  // 格式化后的信息
  LogLevel level;             // 日志级别

  // 流输出操作重载
  friend std::ostream& operator<<(std::ostream& os, const FormatResult& result);
};

// 重载FormatResult的流输出操作
inline std::ostream& operator<<(std::ostream& os, const FormatResult& result) {
  os << result.formatted_msg;
  return os;
}
