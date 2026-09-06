#pragma once
#include <chrono>
#include <string>

#include "logger/config.h"

// 线程安全、跨平台的time_t转tm函数
bool localtime_safe(time_t t, std::tm& tm);

// time_t → UTC struct tm，线程安全
bool gmtime_safe(time_t t, std::tm& tm);

// 日期格式化（time_point → 字符串），线程安全
std::string format_time(const std::chrono::system_clock::time_point& tp, const LogConfig& config);

// 日期时间格式化（含毫秒）：YYYY-MM-DDTHH:MM:SS.mmm，线程安全；文本/JSON 统一走这里
std::string format_time_ms(const std::chrono::system_clock::time_point& tp,
                           const LogConfig& config);

// json字符串转义(处理\"、\\、\b、\f、\t、\r、\n、0x00 ~ 0x1F)
std::string json_escape(const std::string& raw);
