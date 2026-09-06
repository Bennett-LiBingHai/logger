#include "logger/formatter/json_formatter.h"

#include <string>

#include "logger/format.h"
#include "logger/level.h"
#include "logger/utiils.h"

// 追加 JSON 键："key":
void JsonFormatter::append_key(std::string& out, const std::string& key) {
  out += '"';
  out += key;
  out += "\": ";
}

// 格式化Record信息,线程安全
[[nodiscard]] FormatResult JsonFormatter::format(const Record& msg, const LogConfig& config,
                                                 bool less) {
  std::string out;
  out += '{';

  append_key(out, "time");
  encode(format_time_ms(msg.time, config), out, true);
  out += ", ";

  append_key(out, "level");
  encode(to_lower_string(msg.log_level), out, true);
  out += ", ";

  append_key(out, "msg");
  encode(msg.content, out, true);

  // 结构化字段：紧跟 msg，按调用顺序，保类型
  for (const auto& f : msg.fields) {
    out += ", ";
    append_key(out, f.key);
    f.value.encode(out, true);
  }

  // 调试信息（仅宏入口携带文件/行/函数时输出）
  if (!less) {
    out += ", ";
    append_key(out, "thread_id");
    encode(msg.thread_id, out, true);
    out += ", ";
    append_key(out, "file");
    encode(msg.file, out, true);
    out += ", ";
    append_key(out, "line");
    encode(msg.line, out, true);
    out += ", ";
    append_key(out, "func");
    encode(msg.func, out, true);
  }

  out += '}';
  out += '\n';

  return FormatResult{std::move(out), msg.log_level};
}
