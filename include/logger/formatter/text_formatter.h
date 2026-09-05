#pragma once
#include "logger/config.h"
#include "logger/format_result.h"
#include "logger/record.h"

// 文本格式化器
class TextFormatter {
 public:
  // 格式化Record信息,线程安全
  [[nodiscard]] static FormatResult format(const Record& msg, const LogConfig& config);
};
