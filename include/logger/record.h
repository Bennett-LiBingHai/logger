#pragma once
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "logger/field.h"
#include "logger/level.h"

// 日志信息
struct Record {
  std::chrono::system_clock::time_point time;  // 时间戳
  LogLevel log_level;                          // 日志级别
  std::string content;                         // 日志内容（消息正文）
  std::thread::id thread_id;                   // 线程id
  const char* file;                            // 文件名
  int line;                                    // 行数
  const char* func;                            // 函数名
  std::vector<Field> fields;                   // 结构化字段（有序、保类型）
  std::uint64_t repeat_count = 0;  // 消息重复出现的次数（聚合去重填充，> 1 才输出）
};
