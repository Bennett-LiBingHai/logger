#include "logger/detail/utils.h"

#include <ctime>
#include <sstream>

// 线程 id 的文本形式，按 id 缓存。见头文件里的说明
const std::string& thread_id_str(std::thread::id id) {
  // 单槽缓存：同一个线程连续打日志时命中（同步模式必然命中）。
  // 换成按 id 建表会有两个问题：格式化线程上的表会随"曾经出现过的业务线程数"增长，
  // 而线程 id 一旦线程结束就可能被复用；单槽没有这两个问题，最坏只是退回重新格式化。
  static thread_local std::thread::id cached_id{};
  static thread_local std::string cached_str;

  if (cached_str.empty() || cached_id != id) {
    std::ostringstream oss;
    oss << id;
    cached_str = oss.str();
    cached_id = id;
  }
  return cached_str;
}

// 线程安全、跨平台的time_t转tm函数
bool localtime_safe(time_t t, std::tm& tm) {
#if defined(_WIN32)
  return 0 == localtime_s(&tm, &t);
#else
  return nullptr != localtime_r(&t, &tm);
#endif
}

// time_t → UTC struct tm，线程安全
bool gmtime_safe(time_t t, std::tm& tm) {
#if defined(_WIN32)
  // Windows: gmtime_s(tm*, time_t*)
  return 0 == gmtime_s(&tm, &t);
#else
  // POSIX/Linux/macOS: gmtime_r(const time_t*, tm*)
  return nullptr != gmtime_r(&t, &tm);
#endif
}

// 日期格式化,不含微秒数,失败返回空字符串
std::string format_time(const std::chrono::system_clock::time_point& tp, const LogConfig& config) {
  time_t t = std::chrono::system_clock::to_time_t(tp);
  tm tm;
  bool ret;
  if (config.use_utc_time) {
    ret = gmtime_safe(t, tm);
  } else {
    ret = localtime_safe(t, tm);
  }
  if (!ret)
    return "";
  char buf[32];
  switch (config.time_format) {
  case TimeFormat::ISO8601: {
    if (strftime(buf, sizeof(buf), "%FT%T", &tm) == 0)
      return "";
    break;
  }
  default: {
    return "";
  }
  }
  return buf;
}

// 日期时间格式化（含毫秒）：YYYY-MM-DDTHH:MM:SS.mmm，线程安全
std::string format_time_ms(const std::chrono::system_clock::time_point& tp,
                           const LogConfig& config) {
  std::string base = format_time(tp, config);
  if (base.empty())
    return base;
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()) % 1000;
  char buf[8];
  std::snprintf(buf, sizeof(buf), ".%03lld", static_cast<long long>(ms.count()));
  base += buf;
  return base;
}

// json字符串转义(处理\"、\\、\b、\f、\t、\r、\n、0x00 ~ 0x1F)
std::string json_escape(const std::string& raw) {
  std::string out;
  // 预分配内存，减少 realloc，保守放大 1.2 倍（整数运算，不引入浮点收窄）
  out.reserve(raw.size() + raw.size() / 5);

  for (char ch : raw) {
    uint8_t c = static_cast<uint8_t>(ch);
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\b':
      out += "\\b";
      break;
    case '\f':
      out += "\\f";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      // 0x00 ~ 0x1F 不可打印控制字符，统一 \u00XX
      if (c <= 0x1F) {
        // \u00xx 格式，十六进制小写
        char buf[7];
        // sprintf 格式化：\u00 + 两位十六进制
        std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned int>(c));
        out.append(buf);
      } else {
        // 普通可见字符直接追加（中文、emoji原样保留）
        out += ch;
      }
      break;
    }
  }
  return out;
}

// 文本转义：反斜杠与控制字符转成字面转义序列，保证一条日志只占一行。
// 反斜杠一起转义，否则「数据里的反斜杠+n」与「转义后的换行」无法区分。
void append_text_escaped(std::string_view raw, std::string& out) {
  static constexpr char kHex[] = "0123456789abcdef";

  bool need = false;
  for (const char ch : raw) {
    const auto c = static_cast<uint8_t>(ch);
    if (c == '\\' || c < 0x20 || c == 0x7F) {
      need = true;
      break;
    }
  }
  if (!need) {  // 绝大多数日志无需转义，走快路径
    out.append(raw);
    return;
  }

  for (const char ch : raw) {
    const auto c = static_cast<uint8_t>(ch);
    switch (c) {
    case '\\':
      out += "\\\\";
      break;
    case '\b':
      out += "\\b";
      break;
    case '\f':
      out += "\\f";
      break;
    case '\t':
      out += "\\t";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\n':
      out += "\\n";
      break;
    default:
      if (c < 0x20 || c == 0x7F) {  // 其余控制字符：\xNN
        out += "\\x";
        out += kHex[c >> 4];
        out += kHex[c & 0x0F];
      } else {
        out += ch;
      }
      break;
    }
  }
}

std::string text_escape(std::string_view raw) {
  std::string out;
  out.reserve(raw.size());
  append_text_escaped(raw, out);
  return out;
}

// UTF-8 安全截断：退到完整码点边界，避免切出非法 UTF-8；被截断时以 "..." 结尾
void truncate_utf8(std::string& s, std::size_t limit) {
  if (s.size() <= limit)
    return;

  // 预留 "..." 的位置（limit 不足 3 字节时不加标记）
  std::size_t cut = (limit >= 3) ? limit - 3 : limit;
  // UTF-8 续字节形如 10xxxxxx，回退到码点起始处
  while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80)
    --cut;

  s.resize(cut);
  if (limit >= 3)
    s += "...";
}
