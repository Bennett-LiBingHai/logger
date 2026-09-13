#pragma once
#include <chrono>
#include <cstdint>

#include "logger/level.h"

// 日志聚合去重：同一线程内，相同 (level, file, line) 在窗口内只输出首条，
// 序列结束时补一条带重复次数的摘要。
//
// 键用 (level, file, line) 而非消息正文：级别过滤之后立刻可算，不依赖格式化，
// 否则最贵的一步已经做完，聚合就失去意义了。它正好对应「同一处代码在循环里刷屏」。
//
// 状态是 thread_local：热路径零锁，代价是计数按线程而非全局。
// 本文件只做计数与判定，不认识 Logger / Record，便于单独测试。

class DedupFilter {
 public:
  enum class Decision {
    Emit,      // 新序列首条：正常输出
    Suppress,  // 窗口内重复：不输出
  };

  // 登记一次日志调用。
  //   out_prev_count   上一条序列的总次数；> 1 表示调用方需先补发一条计数摘要。
  //   out_need_payload 返回 Suppress 且为 true（即本次是序列的首次重复）时，
  //                    调用方需组装一条摘要载荷供序列结束时使用。
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

    // 序列切换：把上一条的次数交出去
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

  // 结束当前序列；返回其总次数（> 1 表示调用方需补发摘要）
  std::uint64_t take_prev_count() {
    const std::uint64_t n = active_ ? count_ : 0;
    active_ = false;
    count_ = 0;
    return n;
  }

  [[nodiscard]] bool active() const {
    return active_;
  }
  [[nodiscard]] std::uint64_t count() const {
    return count_;
  }

 private:
  bool active_ = false;
  LogLevel level_ = LogLevel::TRACE;
  const char* file_ = nullptr;
  int line_ = 0;
  std::uint64_t count_ = 0;
  std::chrono::steady_clock::time_point window_start_{};
};

// 当前线程的过滤器
inline DedupFilter& dedup_filter() {
  static thread_local DedupFilter filter;
  return filter;
}
