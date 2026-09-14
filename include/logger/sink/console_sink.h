#pragma once
#include "logger/level.h"
#include "logger/sink.h"

/// @brief 控制台输出槽：按级别把日志分流到 stdout 与 stderr。
///
/// 低于 errlevel 的写 stdout，达到或高于的写 stderr。这样在容器或脚本里可以把
/// 正常日志与错误日志分开收集：
/// @code
/// ./app 1>access.log 2>error.log
/// @endcode
///
/// @note 本类不是线程安全的；Logger 在写 Sink 时会加锁，因此无需自行同步。
class ConsoleSink : public LogSink {
 public:
  /// @brief 构造。
  /// @param errlevel 输出到 stderr 的最低级别，默认 ERROR。
  ConsoleSink(LogLevel errlevel = LogLevel::ERROR);

  /// @brief 写入一条日志：按级别选择 stdout 或 stderr。
  /// @param input 格式化成品。
  /// @return 是否写入成功。
  bool log(const SinkInput& input) override;

  /// @brief 刷新输出流缓冲。
  void flush() override;

 private:
  LogLevel errlevel_;  ///< 输出到 stderr 的最低级别
};
