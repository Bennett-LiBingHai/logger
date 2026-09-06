#pragma once
#include <string>

#include "logger/config.h"
#include "logger/formatter.h"
#include "logger/record.h"

// json格式化器
class JsonFormatter {
 public:
  // 格式化Record信息,线程安全
  [[nodiscard]] static FormatResult format(const Record& msg, const LogConfig& config,
                                           bool less = false);

 private:
  // 追加 JSON 键："key":
  static void append_key(std::string& out, const std::string& key);
};
