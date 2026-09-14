#include "logger/detail/formatter/json_formatter.h"

#include <string>

#include "logger/detail/format.h"
#include "logger/detail/utils.h"
#include "logger/level.h"

// 追加 JSON 键："key":
void JsonFormatter::append_key(std::string& out, const std::string& key) {
  out += '"';
  out += key;
  out += "\": ";
}

// 预留长度的经验值，同 TextFormatter：避免拼接过程中反复扩容 + 拷贝
constexpr std::size_t kHeaderReserve = 128;
constexpr std::size_t kPerFieldReserve = 48;

// 格式化Record信息,线程安全
[[nodiscard]] SinkInput JsonFormatter::format(const Record& msg, const LogConfig& config,
                                              bool less) {
  std::string out;
  out.reserve(kHeaderReserve + msg.content.size() + msg.fields.size() * kPerFieldReserve);
  out += '{';

  append_key(out, "time");
  encode(format_time_ms(msg.time, config), out, true);
  out += ", ";

  append_key(out, "level");
  encode(to_lower_string(msg.log_level), out, true);
  out += ", ";

  append_key(out, "msg");
  encode(msg.content, out, true);

  // 聚合摘要：折叠次数作为独立成员，便于下游直接聚合
  if (msg.repeat_count > 1) {
    out += ", ";
    append_key(out, "repeated");
    out += std::to_string(msg.repeat_count);
  }

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
    // 走缓存：encode(std::thread::id) 会落到 operator<< 分支，每条构造一次 ostringstream
    encode(thread_id_str(msg.thread_id), out, true);
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

  return SinkInput{std::move(out), msg.log_level};
}
