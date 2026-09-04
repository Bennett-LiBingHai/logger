#pragma once
#include"logger/config.h"
#include<chrono>
#include<string>

//线程安全、跨平台的time_t转tm函数
bool localtime_safe(time_t t,std::tm& tm);

// time_t → UTC struct tm，线程安全
bool gmtime_safe(time_t t, std::tm& tm);

//日期格式化（time_point → 字符串），线程安全
std::string format_time(const std::chrono::system_clock::time_point& tp,const LogConfig& config);