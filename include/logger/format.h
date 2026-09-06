#pragma once
#include <string>
#include <vector>

#include "logger/field.h"

// 递归终止：参数包为空
inline void collect_args(std::vector<std::string>& /*arg_list*/) {}

// 递归展开可变参数包，逐个编码为文本存入 vector
template <typename T, typename... Args>
void collect_args(std::vector<std::string>& arg_list, const T& first, const Args&... rest) {
  std::string s;
  encode(first, s, false);
  arg_list.emplace_back(std::move(s));
  collect_args(arg_list, rest...);
}

// 支持 {} 的位置参数格式化（消息正文）。Field 类型由调用方（log）先行拆出，不进此函数。
// - {} 数量多于参数：剩余占位符原样保留
// - 参数多于 {}：多余参数忽略
// 不抛异常。
template <typename... Args>
std::string format(const std::string& fmt, const Args&... args) {
  std::vector<std::string> arg_list;
  arg_list.reserve(sizeof...(Args));
  collect_args(arg_list, args...);

  std::string result;
  size_t arg_idx = 0;
  size_t i = 0;
  const size_t n = fmt.size();

  while (i < n) {
    if (i + 1 < n && fmt[i] == '{' && fmt[i + 1] == '}') {
      if (arg_idx < arg_list.size())
        result += arg_list[arg_idx++];
      else
        result += "{}";  // 参数不足：保留原占位符
      i += 2;
    } else {
      result += fmt[i];
      ++i;
    }
  }
  return result;
}
