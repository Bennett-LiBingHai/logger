#pragma once
#include <ostream>
#include <string>

#include "logger/level.h"

namespace logger {
/// @file sink.h
/// @brief 输出槽抽象与交给输出槽的成品。

/// @brief 交给 Sink 的成品：格式化完成的整条记录。
///
/// 自定义 Sink 在 log() 里收到的就是它；要接入自建后端时，把 formatted_msg 直接
/// 写出去即可，无需再关心里面的字段与转义。
struct SinkInput {
  std::string formatted_msg;  ///< 格式化后的整条记录，已含结尾换行
  LogLevel level;             ///< 日志级别，供 Sink 自行分流

  /// @brief 流输出操作重载：等价于直接输出 formatted_msg。
  /// @param os 目标流。
  /// @param input 要输出的成品。
  /// @return 目标流引用。
  friend std::ostream& operator<<(std::ostream& os, const SinkInput& input);
};

/// @brief 重载 SinkInput 的流输出操作。
/// @param os 目标流；@param input 成品。
/// @return 目标流引用。
inline std::ostream& operator<<(std::ostream& os, const SinkInput& input) {
  os << input.formatted_msg;
  return os;
}

/// @brief 输出槽抽象：日志的终点。
///
/// 继承本类即可接入任意后端（自建收集器、消息队列、内存缓冲……）。内置实现见
/// ConsoleSink 与 FileSink。
///
/// @note 实现**不需要**自己加锁：Logger 在调用时会保证同一时刻只有一个线程在写。
/// @warning log() 抛出的异常会被 Logger 捕获并按失败处理（不计入崩溃），但仍应尽量
///          避免抛出——那会丢日志并触发降级。
class LogSink {
 public:
  /// @brief 写出一条日志。
  /// @param input 格式化成品。
  /// @return 写入是否成功；返回 false 时 Logger 按 LogFailStrategy 降级（转 stderr 或丢弃）。
  virtual bool log(const SinkInput& input) = 0;

  /// @brief 刷新内部缓冲，保证已写出的内容落到设备上。
  virtual void flush() = 0;

  virtual ~LogSink() = default;
};

}  // namespace logger
