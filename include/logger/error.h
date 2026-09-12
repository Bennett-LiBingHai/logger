#pragma once
#include <cctype>
#include <cstdlib>
#include <exception>
#include <memory>
#include <string>
#include <typeinfo>

#if defined(__GNUG__)
#include <cxxabi.h>
#endif

// ===== 异常信息提取（M5：错误记录）=====
// 把一个异常（含 std::throw_with_nested 的嵌套链）转成可读的 message/type/chain，
// 供 LOG_EXCEPTION 自动展开为 error / error_type / error_chain 字段。

// 反解符号名：abi::__cxa_demangle 失败时原样返回 mangled 名
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

// throw_with_nested 抛出的实际类型是实现内部的包装（libstdc++: std::_Nested_exception<T>，
// libc++: std::__nested_exception<T>），对用户无意义。剥掉包装，暴露真正的异常类型 T。
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

    // 按尖括号配对取模板实参（实参本身可能是嵌套模板）
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

// 异常信息：message/type 取最内层异常，chain 为完整嵌套链
struct ExceptionInfo {
  std::string message;      // 最内层 what()
  std::string type;         // 最内层可读类型名（demangle 后）
  std::string chain;        // 完整链："type: msg" 或 "type: msg\n  caused by: ..."
  bool has_nested = false;  // 是否存在嵌套异常
};

// 递归：把异常 e 写入 chain，并把最内层 message/type 写回
inline void append_exception(const std::exception& e, std::string& chain,
                             std::string& innermost_msg, std::string& innermost_type) {
  // typeid 取动态类型（std::exception 有多态性）→ demangle 成可读名 → 剥掉嵌套包装
  const std::string type = normalize_type(demangle(typeid(e).name()));
  chain += type;
  chain += ": ";
  chain += e.what();
  innermost_msg = e.what();
  innermost_type = type;

  try {
    std::rethrow_if_nested(e);
  } catch (const std::exception& nested) {
    chain += "\n  caused by: ";
    append_exception(nested, chain, innermost_msg, innermost_type);
  } catch (...) {
    chain += "\n  caused by: <unknown>";
  }
}

// 提取异常信息
inline ExceptionInfo extract_exception(const std::exception& e) {
  ExceptionInfo info;
  append_exception(e, info.chain, info.message, info.type);
  info.has_nested = info.chain.find("\n  caused by:") != std::string::npos;
  return info;
}

// exception_ptr 版本：空指针给占位，非空 rethrow 后复用上面
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
