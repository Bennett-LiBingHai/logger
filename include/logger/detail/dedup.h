#pragma once
#include <chrono>
#include <cstdint>

#include "logger/level.h"

/// @file detail/dedup.h
/// @brief 聚合去重：同一线程内，相同来源的重复日志折叠成「首条 + 次数摘要」。
///
/// 内部实现，不承诺接口稳定；行为由 LogConfig::dedup_window_ms 控制。
///
/// @par 为什么键用 (level, file, line) 而不是消息正文
/// 级别过滤之后立刻可算，不依赖格式化 —— 否则最贵的一步已经做完，聚合就失去意义了。
/// 而且它正好对应「同一处代码在循环里刷屏」这个真实场景。
///
/// @par 为什么状态是 thread_local
/// 热路径零锁。代价是计数按线程而非全局，对「防止噪声淹没有效日志」这个目的够用。
///
/// 本文件只做计数与判定，不认识 Logger / Record，因此可以单独测试。

/// @brief 聚合去重的判定状态机。
class DedupFilter {
 public:
  /// @brief 单次调用的处置结果。
  enum class Decision : std::uint8_t {
    Emit,      ///< 新序列的首条：正常输出
    Suppress,  ///< 窗口内的重复：不输出
  };

  /// @brief 登记一次日志调用并给出处置结果。
  /// @param level 日志级别。
  /// @param file 源文件名（调用点）。
  /// @param line 源文件行号（调用点）。
  /// @param window_ms 聚合窗口（毫秒）；为 0 表示未启用，一律 Emit。
  /// @param out_prev_count 输出参数：上一条序列的总次数；> 1 表示调用方需先补发一条摘要。
  /// @param out_need_payload 输出参数：返回 Suppress 且为 true（本次是序列的首次重复）时，
  ///        调用方需组装一条摘要载荷，供序列结束时使用。
  /// @return 本条日志的处置结果。
  Decision on_log(LogLevel level, const char* file, int line, std::uint64_t window_ms,
                  std::uint64_t* out_prev_count, bool* out_need_payload) {
    if (out_prev_count != nullptr)
      *out_prev_count = 0;
    if (out_need_payload != nullptr)
      *out_need_payload = false;
    if (window_ms == 0)
      return Decision::Emit;

    const auto now = std::chrono::steady_clock::now();
    const bool same = active_ && level_ == level && file_ == file && line_ == line &&
                      now - window_start_ < std::chrono::milliseconds(window_ms);
    if (same) {
      const bool first_repeat = (count_ == 1);
      ++count_;
      if (out_need_payload != nullptr)
        *out_need_payload = first_repeat;
      return Decision::Suppress;
    }

    // 序列切换：把上一条的次数交出去，再开启新序列
    if (out_prev_count != nullptr && active_)
      *out_prev_count = count_;

    active_ = true;
    level_ = level;
    file_ = file;
    line_ = line;
    count_ = 1;
    window_start_ = now;
    return Decision::Emit;
  }

  /// @brief 结束当前序列。
  /// @return 该序列的总次数；> 1 表示调用方需补发摘要。调用后状态复位。
  std::uint64_t take_prev_count() {
    const std::uint64_t n = active_ ? count_ : 0;
    active_ = false;
    count_ = 0;
    return n;
  }

  /// @brief 当前是否有正在累积的序列。
  /// @return true 表示有。
  [[nodiscard]] bool active() const {
    return active_;
  }

  /// @brief 当前序列已累积的次数（含首条）。
  /// @return 次数。
  [[nodiscard]] std::uint64_t count() const {
    return count_;
  }

 private:
  bool active_ = false;               ///< 是否有正在累积的序列
  LogLevel level_ = LogLevel::TRACE;  ///< 当前序列的级别（键的一部分）
  const char* file_ = nullptr;        ///< 当前序列的源文件（键的一部分）
  int line_ = 0;                      ///< 当前序列的行号（键的一部分）
  std::uint64_t count_ = 0;           ///< 当前序列已累积的次数
  std::chrono::steady_clock::time_point window_start_{};  ///< 当前窗口的起点
};

/// @brief 当前线程的聚合去重状态机。
/// @return 过滤器引用（thread_local）。
inline DedupFilter& dedup_filter() {
  static thread_local DedupFilter filter;
  return filter;
}
