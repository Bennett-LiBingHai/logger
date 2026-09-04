#pragma once
#include"logger/level.h"
#include<string>
#include<thread>
#include<chrono>

//日志信息
struct Record{
    std::chrono::system_clock::time_point time;//时间戳
    LogLevel log_level;//日志级别
    std::string content;//日志内容
    std::thread::id thread_id;//线程id
    const char* file;//文件名
    int line;//行数
    const char* func;//函数名
};