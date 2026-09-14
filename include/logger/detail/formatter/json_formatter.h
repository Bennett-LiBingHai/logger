#pragma once
#include <string>

#include "logger/config.h"
#include "logger/detail/record.h"
#include "logger/sink.h"

/// @file detail/formatter/json_formatter.h
/// @brief JSON 格式：一条记录一行，便于下游按字段检索。
///
/// 内部实现，不承诺接口稳定；用户通过 LogConfig::format 选择格式，不直接调用。

/// @brief JSON 格式化器。
///
/// 输出形如：
/// @code
/// {"time": "...", "level": "info", "msg": "user login", "user_id": 1001}
/// @endcode
///
/// 固定成员为 `time` / `level` / `msg`，随后是用户字段（按调用顺序），
/// 结构化入口还会附带线程 / 文件行 / 函数。字段保留类型：数字不加引号。
///
/// @par 单行不变式
/// 所有字符串都经 json_escape 转义，因此记录本身是合法 JSON 且只占一行 ——
/// 可直接被 JSONL 采集器逐行解析。
///
/// @par 无状态
/// 只有静态方法，可被多线程并发调用。
class JsonFormatter {
 public:
  /// @brief 格式化一条记录。
  /// @param msg 记录。
  /// @param config 配置（时间格式、时区、记录长度上限等）。
  /// @param less true 表示结构化入口，不输出线程 / 文件行 / 函数。
  /// @return 交给 Sink 的成品；长度受 config.max_record_size 约束。
  /// @note 线程安全，无内部状态。
  [[nodiscard]] static SinkInput format(const Record& msg, const LogConfig& config,
                                        bool less = false);

 private:
  /// @brief 追加 JSON 键，形如 `"key": `。
  /// @param out 输出缓冲。
  /// @param key 键名（不含引号）。
  static void append_key(std::string& out, const std::string& key);
};
