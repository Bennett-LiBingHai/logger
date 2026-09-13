#include "logger/formatter/text_formatter.h"

#include <sstream>
#include <string>

#include "logger/utiils.h"

// 格式化Record信息,线程安全
[[nodiscard]] FormatResult TextFormatter::format(const Record& msg, const LogConfig& config,
                                                 bool less) {
  std::string body = format_time_ms(msg.time, config);
  body += '[';
  body += to_string(msg.log_level);
  body += ']';
  if (!less) {
    std::ostringstream oss;
    oss << '[' << msg.thread_id << ']' << '[' << msg.file << ':' << msg.line << ']' << '['
        << msg.func << ']';
    body += oss.str();
  }
  body += msg.content;
  if (msg.repeat_count > 1) {  // 聚合摘要：紧跟正文标注折叠次数
    body += " (repeated ";
    body += std::to_string(msg.repeat_count);
    body += " times)";
  }
  for (const auto& f : msg.fields) {
    std::string v;
    f.value.encode(v, false);
    body += ' ';
    body += f.key;
    body += '=';
    body += v;
  }

  // 一条记录只占一行：正文里的控制字符统一转义，末尾只留一个 '\n'
  std::string line;
  line.reserve(body.size() + 1);
  append_text_escaped(body, line);
  line += '\n';

  return FormatResult{std::move(line), msg.log_level};
}
