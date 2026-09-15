#pragma once
#include <cctype>
#include <cstdlib>
#include <exception>
#include <memory>
#include <string>
#include <typeinfo>

#if defined(__GNUG__)
#include <cxxabi.h>

namespace logger::detail {
#endif

/// @file detail/error.h
/// @brief 异常信息提取：把一个异常（含嵌套链）转成可读的 message / type / chain。
///
/// 内部实现，不承诺接口稳定；用户通过 LOG_EXCEPTION 使用，不直接调用这些函数。

/// @brief 反解 C++ 符号名。
/// @param mangled ABI 编码名（如 typeid().name() 的结果）。
/// @return 可读的 C++ 名；反解失败或平台不支持时原样返回 mangled 名。
inline std::string demangle(const char* mangled) {
  if (mangled == nullptr)
    return "unknown";
#if defined(__GNUG__)
  int status = -1;
  std::unique_ptr<char, decltype(&std::free)> out(
      abi::__cxa_demangle(mangled, nullptr, nullptr, &status), &std::free);
  if (status == 0 && out)
    return out.get();
#endif
  return mangled;
}

/// @brief 剥掉 throw_with_nested 的内部包装，暴露真正的异常类型。
///
/// `std::throw_with_nested` 抛出的实际类型是实现内部的包装
/// （libstdc++ 是 `std::_Nested_exception<T>`，libc++ 是 `std::__nested_exception<T>`），
/// 对使用者没有意义，必须剥掉。
///
/// @param name 已 demangle 的类型名。
/// @return 剥掉包装后的类型名；没有包装时原样返回。
inline std::string normalize_type(const std::string& name) {
  std::string cur = name;
  for (;;) {
    // 定位 "nested_exception"（大小写不敏感，兼容两个标准库的命名），再找其后的 '<'
    std::size_t pos = std::string::npos;
    for (std::size_t i = 0; i + 16 <= cur.size(); ++i) {
      bool hit = true;
      for (std::size_t k = 0; k < 16; ++k) {
        const char c = cur[i + k];
        if (std::tolower(static_cast<unsigned char>(c)) != "nested_exception"[k]) {
          hit = false;
          break;
        }
      }
      if (hit) {
        pos = i;
        break;
      }
    }
    if (pos == std::string::npos)
      return cur;

    const std::size_t open = cur.find('<', pos);
    if (open == std::string::npos)
      return cur;

    // 按尖括号配对取模板实参（实参本身可能是嵌套模板），再递归剥一层
    int depth = 0;
    for (std::size_t i = open; i < cur.size(); ++i) {
      if (cur[i] == '<')
        ++depth;
      else if (cur[i] == '>' && --depth == 0)
        return normalize_type(cur.substr(open + 1, i - open - 1));
    }
    return cur;
  }
}

/// @brief 提取出的异常信息。
///
/// `message` 与 `type` 取**最内层**（即根因），`chain` 是从外层到内层的完整链。
struct ExceptionInfo {
  std::string message;      ///< 最内层异常的 what()
  std::string type;         ///< 最内层异常的可读类型名（demangle 并剥包装后）
  std::string chain;        ///< 完整链：`type: msg` 或 `type: msg\n  caused by: ...`
  bool has_nested = false;  ///< 是否存在嵌套异常（决定要不要输出 error_chain 字段）
};

/// @brief 递归展开一个异常：写入 chain，并把最内层的 message / type 回传。
/// @param e 当前层的异常。
/// @param chain 输出参数，累积完整链。
/// @param innermost_msg 输出参数，最内层异常的 what()。
/// @param innermost_type 输出参数，最内层异常的类型名。
/// @note typeid 取的是动态类型（std::exception 有多态性），因此不会退化成基类名。
inline void append_exception(const std::exception& e, std::string& chain,
                             std::string& innermost_msg, std::string& innermost_type) {
  const std::string type = normalize_type(demangle(typeid(e).name()));
  chain += type;
  chain += ": ";
  chain += e.what();
  innermost_msg = e.what();
  innermost_type = type;

  try {
    std::rethrow_if_nested(e);  // 标准机制：有嵌套就继续展开
  } catch (const std::exception& nested) {
    chain += "\n  caused by: ";
    append_exception(nested, chain, innermost_msg, innermost_type);
  } catch (...) {
    chain += "\n  caused by: <unknown>";
  }
}

/// @brief 提取异常信息。
/// @param e 异常对象。
/// @return 提取结果。
inline ExceptionInfo extract_exception(const std::exception& e) {
  ExceptionInfo info;
  append_exception(e, info.chain, info.message, info.type);
  info.has_nested = info.chain.find("\n  caused by:") != std::string::npos;
  return info;
}

/// @brief 提取异常信息（exception_ptr 版本）。
/// @param ep 异常指针；空指针返回占位内容而不是崩溃。
/// @return 提取结果。
inline ExceptionInfo extract_exception(const std::exception_ptr& ep) {
  if (!ep)
    return ExceptionInfo{"<no exception>", "std::exception_ptr", "<no exception>", false};
  try {
    std::rethrow_exception(ep);
  } catch (const std::exception& e) {
    return extract_exception(e);
  } catch (...) {
    return ExceptionInfo{"<unknown>", "<unknown>", "<unknown>", false};
  }
}

}  // namespace logger::detail
