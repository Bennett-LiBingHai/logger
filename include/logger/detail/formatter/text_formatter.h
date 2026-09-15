#pragma once
#include "logger/config.h"
#include "logger/detail/record.h"
#include "logger/sink.h"

namespace logger::detail {
/// @file detail/formatter/text_formatter.h
/// @brief 文本格式：一条记录一行。
///
/// 内部实现，不承诺接口稳定；用户通过 LogConfig::format 选择格式，不直接调用。

/// @brief 文本格式化器。
///
/// 输出形如：
/// @code
/// 2026-09-02T10:20:30.123[INFO][140234][main.cpp:42][handle]user login user_id=1001
/// @endcode
///
/// 组成：时间 → 级别 → 线程 / 文件行 / 函数（结构化入口省略）→ 正文 → 字段。
///
/// @par 单行不变式
/// 正文与字段里的控制字符会被统一转义（见 text_escape），保证整条记录只占一行 ——
/// 下游按 `\n` 切分时不会把一条日志读成多条，用户输入也无法伪造日志行。
///
/// @par 无状态
/// 只有静态方法，可被多线程并发调用。
class TextFormatter {
 public:
  /// @brief 格式化一条记录。
  /// @param msg 记录。
  /// @param config 配置（时间格式、时区、记录长度上限等）。
  /// @param less true 表示结构化入口，不输出线程 / 文件行 / 函数。
  /// @return 交给 Sink 的成品；长度受 config.max_record_size 约束。
  /// @note 线程安全，无内部状态。
  [[nodiscard]] static SinkInput format(const Record& msg, const LogConfig& config,
                                        bool less = false);
};

}  // namespace logger::detail
