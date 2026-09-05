#include "logger/formatter/text_formatter.h"

#include <iomanip>
#include <iostream>
#include <sstream>

#include "logger/utiils.h"

// 格式化Record信息,线程安全
[[nodiscard]] FormatResult TextFormatter::format(const Record& msg, const LogConfig& config) {
  std::string daytime = format_time(msg.time, config);
  std::ostringstream oss;
  if (daytime.empty()) {
    std::cerr << "TextFormatter::format: localtime_safe error\n";
    oss << "unknown time" << '[' << to_string(msg.log_level) << ']' << '[' << msg.thread_id << ']'
        << '[' << msg.file << ':' << msg.line << ']' << msg.content << '\n';
  } else {
    auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(msg.time.time_since_epoch()) % 1000;
    oss << daytime << '.' << std::setw(3) << std::setfill('0') << ms.count() << '['
        << to_string(msg.log_level) << ']' << '[' << msg.thread_id << ']' << '[' << msg.file << ':'
        << msg.line << ']' << msg.content << '\n';
  }
  FormatResult result{oss.str(), msg.log_level};
  return result;
}
