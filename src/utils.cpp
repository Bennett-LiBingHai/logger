#include <ctime>

#include "logger/utiils.h"

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
  // 预分配内存，减少realloc，保守放大1.2倍
  out.reserve(static_cast<size_t>(raw.size() * 1.2));

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
