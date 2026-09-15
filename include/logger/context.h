#pragma once
#include "logger/field.h"

namespace logger {
/// @file context.h
/// @brief 作用域上下文（ContextScope）及其 thread_local 栈。

/// @brief 作用域上下文：进入作用域后，本线程打出的日志自动附加这些字段。
///
/// 基于 thread_local 栈 + RAII：构造入栈、析构出栈，天然支持嵌套（内层覆盖外层同名
/// 字段）。作用域内所有日志——包括 `LOG_*` 宏打出的——都自动带上这些字段，
/// 无需把 Logger 对象沿调用链传下去。
///
/// @par 跨线程
/// thread_local 不跨线程：子线程 / 线程池拿不到父线程的上下文。需要延续时用
/// `Logger::with(...)` 显式传值。
///
/// @par 字段优先级
/// 调用点的显式 KV 高于本作用域；本作用域高于 `Logger::with()` 预绑定的字段。
///
/// @par 典型用法
/// @code
/// {
///   ContextScope ctx{KV("request_id", req.id), KV("trace_id", req.trace)};
///   LOG_INFO("handling request");  // 自动带上 request_id / trace_id
/// }  // 出作用域，字段自动出栈
/// @endcode
class ContextScope {
 public:
  /// @brief 构造并入栈。
  /// @param args 一个或多个 KV(...) 字段。
  template <typename... Args>
  explicit ContextScope(Args&&... args) {
    split_fields(data_, std::forward<Args>(args)...);
    context_stack().push_back(std::move(data_));  // 构造入栈，析构出栈
  }

  /// @brief 析构并出栈。LIFO 顺序保证嵌套作用域能正确还原。
  ~ContextScope() {
    context_stack().pop_back();
  }

  /// @brief 把本线程上下文中所有帧的字段按入栈顺序追加到 v。
  /// @param v 目标字段列表。
  /// @note 供 Logger 内部在合并字段时调用，用户无需直接使用。
  static void merge_into(std::vector<Field>& v) {
    for (const auto& frame : context_stack())
      v.insert(v.end(), frame.begin(), frame.end());
  }

 private:
  ContextScope(const ContextScope&) = delete;
  ContextScope& operator=(const ContextScope&) = delete;
  ContextScope(ContextScope&&) = delete;
  ContextScope& operator=(ContextScope&&) = delete;

  /// @brief 当前线程的上下文栈（thread_local，天然单实例）。
  /// @return 栈引用，每层是作用域的一帧字段。
  static std::vector<std::vector<Field>>& context_stack() {
    thread_local static std::vector<std::vector<Field>> s;
    return s;
  }

  std::vector<Field> data_;  ///< 本作用域的字段
};

}  // namespace logger
