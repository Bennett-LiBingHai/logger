#pragma once
#include <cstddef>
#include <string>
#include <vector>

/// @brief 调用栈采集：采集与符号化分离。
///
/// capture() 只调用 backtrace() 拿返回地址列表（快、不分配）；str() 才把地址
/// 符号化（dladdr + demangle，慢），并把结果缓存起来。
///
/// 这个拆分带来的实际收益：异步模式下符号化天然发生在后台写入线程，业务线程
/// 只付采集开销；同步模式下 str() 在格式化时调用一次，之后走缓存。
///
/// 显式请求堆栈就是普通字段，不是特殊机制：
/// @code
/// LOG_ERROR("query failed", KV("stacktrace", StackTrace::capture()));
/// @endcode
///
/// @note 不承诺线程安全：同一对象同一时刻只应由一个线程编码。可拷贝 / 移动，
///       以便随记录一起进入异步队列。
class StackTrace {
 public:
  StackTrace() = default;

  /// @brief 采集当前调用栈（只记录地址，不做符号化）。
  /// @param skip 从栈顶丢弃的帧数（含 capture 自身），默认 1。
  /// @param depth 最多采集的帧数。
  /// @param max_length 渲染后的字节上限，0 表示不限。超出时按帧丢弃并在末尾附
  ///                   `... (+N frames)` —— 保证堆栈不霸占整条记录的
  ///                   max_record_size，也不会被字节级硬切。
  /// @return 采集结果。平台不支持时返回空对象（empty() 为 true）。
  [[nodiscard]] static StackTrace capture(std::size_t skip = 1, std::size_t depth = 10,
                                          std::size_t max_length = 512);

  /// @brief 惰性符号化，结果缓存（受 max_length 约束）。
  /// @return 多行渲染结果；无帧时为空串。首次调用付出符号化开销，之后直接返回缓存。
  [[nodiscard]] const std::string& str() const;

  /// @brief 是否没有采集到任何帧。
  /// @return true 表示空。
  [[nodiscard]] bool empty() const {
    return frames_.empty();
  }

 private:
  std::vector<void*> frames_;        ///< 返回地址（未符号化）
  std::size_t max_length_ = 0;       ///< 渲染字节上限，0 = 不限
  mutable std::string cached_;       ///< 符号化结果缓存
  mutable bool symbolized_ = false;  ///< 缓存是否已生成
};
