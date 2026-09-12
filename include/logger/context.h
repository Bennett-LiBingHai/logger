#pragma once
#include "logger/field.h"

// 上下文,作用域内Log自动附加上下文字段
class ContextScope {
 public:
  template <typename... Args>
  explicit ContextScope(Args&&... args) {
    split_fields(data_, std::forward<Args>(args)...);
    context_stack().push_back(std::move(data_));  // 构造入栈，析构出栈（LIFO 保证嵌套正确）
  }

  ~ContextScope() {
    context_stack().pop_back();
  }

  // 合并当前上下文到v：按入栈顺序展开各帧的字段
  static void merge_into(std::vector<Field>& v) {
    for (const auto& frame : context_stack())
      v.insert(v.end(), frame.begin(), frame.end());
  }

 private:
  ContextScope(const ContextScope&) = delete;
  ContextScope& operator=(const ContextScope&) = delete;
  ContextScope(ContextScope&&) = delete;
  ContextScope& operator=(ContextScope&&) = delete;

  // 单线程全局共享上下文
  static std::vector<std::vector<Field>>& context_stack() {
    thread_local static std::vector<std::vector<Field>> s;
    return s;
  }

  std::vector<Field> data_;  // 上下文信息
};
