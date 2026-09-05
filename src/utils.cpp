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
