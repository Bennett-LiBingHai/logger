#pragma once
#include <string>
#include <vector>

#include "logger/field.h"

/// @file detail/format.h
/// @brief `{}` 占位符的位置参数格式化。
///
/// 内部实现，不承诺接口稳定。只处理消息正文；Field / KV(...) 由调用方先行拆出，
/// 不进入本函数（见 field.h 的 split_fields）。

/// @brief 可变参数收集的递归终止。
/// @param arg_list 未使用。
inline void collect_args(std::vector<std::string>& /*arg_list*/) {}

/// @brief 递归展开可变参数包，逐个按文本编码后存入 vector。
/// @tparam T 首个参数类型。
/// @tparam Args 其余参数类型。
/// @param arg_list 输出参数，收集编码后的字符串。
/// @param first 首个参数。
/// @param rest 其余参数。
template <typename T, typename... Args>
void collect_args(std::vector<std::string>& arg_list, const T& first, const Args&... rest) {
  std::string s;
  encode(first, s, false);  // 用文本编码：消息正文是格式无关的原始数据，转义留给 Formatter
  arg_list.emplace_back(std::move(s));
  collect_args(arg_list, rest...);
}

/// @brief 按顺序把位置参数插入 `{}` 占位符，得到消息正文。
/// @tparam Args 位置参数类型。
/// @param fmt 格式串。
/// @param args 依次对应各 `{}` 的参数。
/// @return 插入后的正文。
///
/// - `{}` 多于参数：多余的占位符原样保留
/// - 参数多于 `{}`：多余的参数忽略
/// - 参数值为空指针、容器等按各自 encode 重载处理
///
/// @note 不抛异常；正文里不会出现裸换行（转义在 Formatter 出口统一处理）。
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
      result += fmt[i];  // 格式串本身逐字符拷贝，不转义
      ++i;
    }
  }
  return result;
}
