#pragma once
#include "logger/formatter.h"
#include "logger/literals.h"

// 日志输出槽
class LogSink {
 public:
  // 打印日志,输入格式化后的信息；返回是否写入成功（失败由 Logger 按策略兜底）
  virtual bool log(const FormatResult& result) = 0;
  // 刷新日志缓冲区
  virtual void flush() = 0;

  virtual ~LogSink() = default;
};
