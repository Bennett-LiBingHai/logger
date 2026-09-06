#include "logger/formatter/text_formatter.h"

#include <sstream>
#include <string>

#include "logger/utiils.h"

// 格式化Record信息,线程安全
[[nodiscard]] FormatResult TextFormatter::format(const Record& msg, const LogConfig& config,
                                                 bool less) {
  std::string line = format_time_ms(msg.time, config);
  line += '[';
  line += to_string(msg.log_level);
  line += ']';
  if (!less) {
    std::ostringstream oss;
    oss << '[' << msg.thread_id << ']' << '[' << msg.file << ':' << msg.line << ']' << '['
        << msg.func << ']';
    line += oss.str();
  }
  line += msg.content;
  for (const auto& f : msg.fields) {
    std::string v;
    f.value.encode(v, false);
    line += ' ';
    line += f.key;
    line += '=';
    line += v;
  }
  line += '\n';

  return FormatResult{std::move(line), msg.log_level};
}
