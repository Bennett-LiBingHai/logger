#pragma once
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "logger/utiils.h"

// ===== 文本 / JSON 共用的底层编码入口 =====
// 所有 encode 重载把 value 追加编码到 out；json=true 时字符串加引号 + 转义，数字/布尔原样。
// 注意顺序：字符串系列放最前，其余委托到它们的重载（char、streamable）放在其后。

// 字符串（string_view / string / const char*）
inline void encode(std::string_view v, std::string& o, bool json) {
  if (json) {
    o += '"';
    o.append(json_escape(std::string(v)));
    o += '"';
  } else {
    o.append(v);
  }
}

inline void encode(const std::string& v, std::string& o, bool json) {
  encode(std::string_view(v), o, json);
}

inline void encode(const char* v, std::string& o, bool json) {
  if (!v) {
    o += json ? "null" : "(null)";
    return;
  }
  encode(std::string_view(v), o, json);
}

// 布尔值
inline void encode(bool v, std::string& o, bool /*json*/) {
  o += v ? "true" : "false";
}

// 单个字符：按单字符字符串处理（JSON 下带引号）
inline void encode(char v, std::string& o, bool json) {
  encode(std::string(1, v), o, json);
}

// 整数（各宽度有符号/无符号），排除 bool 与字符类型
template <typename T,
          std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool> &&
                               !std::is_same_v<T, char> && !std::is_same_v<T, wchar_t> &&
                               !std::is_same_v<T, char16_t> && !std::is_same_v<T, char32_t>,
                           int> = 0>
void encode(T v, std::string& o, bool /*json*/) {
  o += std::to_string(v);
}

// 浮点数：%g 最短表示，正确处理 nan / inf（JSON 下 nan/inf 输出 null 保证合法）
inline void encode(double v, std::string& o, bool json) {
  if (std::isnan(v)) {
    o += json ? "null" : "nan";
    return;
  }
  if (std::isinf(v)) {
    o += json ? "null" : (v > 0 ? "inf" : "-inf");
    return;
  }
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.6g", v);
  o += buf;
}

inline void encode(float v, std::string& o, bool json) {
  encode(static_cast<double>(v), o, json);
}

// 数组 / 容器：JSON 下 [a,b,c]，文本下 [a, b, c]
template <typename T>
void encode(const std::vector<T>& v, std::string& o, bool json) {
  o += '[';
  bool first = true;
  for (const auto& e : v) {
    if (!first)
      o += json ? "," : ", ";
    first = false;
    encode(e, o, json);
  }
  o += ']';
}

// 时间点：ISO8601 本地时间（含毫秒），定义在 field.cpp
void encode(std::chrono::system_clock::time_point v, std::string& o, bool json);

// 自定义序列化类型：具有 operator<< 的类型回退到流式输出（文本原样，JSON 按字符串）
template <typename T, std::enable_if_t<!std::is_arithmetic_v<T> && !std::is_enum_v<T>, int> = 0,
          typename = decltype(std::declval<std::ostream&>() << std::declval<const T&>())>
void encode(const T& v, std::string& o, bool json) {
  std::ostringstream oss;
  oss << v;
  encode(oss.str(), o, json);
}

// 便捷入口：返回编码后的字符串
template <typename T>
std::string encode(T&& v, bool json) {
  std::string s;
  encode(std::forward<T>(v), s, json);
  return s;
}

// ===== 键值对字段 =====

// 键值对值类型（类型擦除：持有拷贝的强类型值 + 对应编码器）
class FieldValue {
 public:
  // 根据值获取字段值对象（值独立于调用点，拷贝/移动进来）
  template <class T>
  static FieldValue from(T&& v);

  // 追加当前值的字符串编码到 out
  void encode(std::string& out, bool json) const;

  FieldValue() = default;
  FieldValue(FieldValue&& o) noexcept;
  FieldValue& operator=(FieldValue&& o) noexcept;
  FieldValue(const FieldValue& o);
  FieldValue& operator=(const FieldValue& o);
  ~FieldValue();

 private:
  // 交换资源
  void swap(FieldValue& o) noexcept;

  void* obj_ = nullptr;                                             // 存储的值指针
  void (*encode_)(const void*, std::string&, bool json) = nullptr;  // 对应值类型的编码器
  void* (*clone_)(const void*) = nullptr;  // 对应值类型的克隆函数
  void (*destroy_)(void*) = nullptr;       // 对应值类型的析构函数
};

// 键值对字段
struct Field {
  std::string key;
  FieldValue value;
};

// 字符串字面量 key：编译期拦截空 key
template <std::size_t N, class T>
Field KV(const char (&key)[N], T&& v) {
  static_assert(N > 1, "KV key must not be empty");
  return Field{std::string(key, N - 1), FieldValue::from(std::forward<T>(v))};
}

// 运行期 key（如从变量来）：空 key 在 log 阶段被跳过（见 logger.h split_fields）
template <class T>
Field KV(std::string_view key, T&& v) {
  return Field{std::string(key), FieldValue::from(std::forward<T>(v))};
}

// 根据值获取字段值对象
template <class T>
FieldValue FieldValue::from(T&& v) {
  FieldValue fv;
  using U = std::decay_t<T>;
  fv.obj_ = new U(std::forward<T>(v));  // 拷贝/移动进来，值独立于调用点
  fv.encode_ = [](const void* p, std::string& out, bool json) {
    ::encode(*static_cast<const U*>(p), out, json);
  };
  fv.clone_ = [](const void* p) -> void* { return new U(*static_cast<const U*>(p)); };
  fv.destroy_ = [](void* p) { delete static_cast<U*>(p); };
  return fv;
}
