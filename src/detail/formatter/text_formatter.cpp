#include "logger/detail/formatter/text_formatter.h"

#include <string>

#include "logger/detail/utils.h"

// 预留长度的经验值：头部（时间戳 + 级别 + 线程 + 文件行 + 函数）约 96 字节，
// 每个字段（key + 编码后的值）按 48 字节估。估小了只是多一次扩容，估大了浪费内存，
// 数量级对就行 —— 目的是避免一条百来字节的记录在拼接过程中反复扩容 + 拷贝。
constexpr std::size_t kHeaderReserve = 96;
constexpr std::size_t kPerFieldReserve = 48;

// 格式化Record信息,线程安全
[[nodiscard]] SinkInput TextFormatter::format(const Record& msg, const LogConfig& config,
                                              bool less) {
  std::string body;
  body.reserve(kHeaderReserve + msg.content.size() + msg.fields.size() * kPerFieldReserve);
  body += format_time_ms(msg.time, config);
  body += '[';
  body += to_string(msg.log_level);
  body += ']';
  if (!less) {
    // 用 thread_id_str 取缓存的线程 id 文本：这里原本是一个 ostringstream，
    // 只为把四个部件拼起来就付一次流构造的开销（实测约 480 ns/条）
    body += '[';
    body += thread_id_str(msg.thread_id);
    body += "][";
    body += msg.file != nullptr ? msg.file : "";
    body += ':';
    body += std::to_string(msg.line);
    body += "][";
    body += msg.func != nullptr ? msg.func : "";
    body += ']';
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

  return SinkInput{std::move(line), msg.log_level};
}
