#pragma once
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "logger/field.h"
#include "logger/level.h"

/// @file detail/record.h
/// @brief 一条日志记录的数据模型。
///
/// 内部类型，不承诺接口稳定；用户通过 LOG_* 宏或 Logger 接口产生记录，不直接构造。

/// @brief 一条日志记录：日志器内部流转的全部信息。
///
/// 记录在业务线程上物化完成后，才会进入异步队列或直接写出；因此它必须自包含 ——
/// 不引用调用点的临时对象，字段值也已按值持有。
struct Record {
  std::chrono::system_clock::time_point time;  ///< 时间戳（业务线程打点，用于算写出延迟）
  LogLevel log_level;                          ///< 日志级别
  std::string content;        ///< 消息正文（已插入位置参数，未编码）
  std::thread::id thread_id;  ///< 产生这条日志的线程
  const char* file;           ///< 源文件名；结构化入口为 nullptr
  int line;                   ///< 源文件行号；结构化入口为 0
  const char* func;           ///< 函数名；结构化入口为 nullptr
  std::vector<Field> fields;  ///< 结构化字段（有序、保留类型）
  std::uint64_t repeat_count = 0;  ///< 消息重复出现的次数（聚合去重填充，> 1 才输出）
};
