#pragma once
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "logger/detail/utils.h"
#include "logger/stacktrace.h"

namespace logger {
/// @file field.h
/// @brief 文本 / JSON 共用的底层编码入口，以及结构化字段 Field 与 KV()。

/// @defgroup encode 编码入口
/// 所有 encode 重载把 value 追加编码到 out：json=true 时字符串加引号并转义，
/// 数字与布尔原样输出（保留类型）。
///
/// 文本与 JSON 走同一条路径，只有字符串分支不同 —— 这是"同一个值在两种格式下只是
/// 序列化形式不同、语义一致"的保证。自定义类型通过重载本函数接入。
/// @{

/// @brief 编码字符串；JSON 下加引号并转义，文本下原样输出。
/// @param v 待编码的值。
/// @param o 输出缓冲，追加写入。
/// @param json 是否为 JSON 格式。
inline void encode(std::string_view v, std::string& o, bool json) {
  if (json) {
    o += '"';
    o.append(detail::json_escape(std::string(v)));
    o += '"';
  } else {
    o.append(v);
  }
}

/// @brief 编码 std::string。
/// @param v 待编码的值；@param o 输出缓冲；@param json 是否为 JSON 格式。
inline void encode(const std::string& v, std::string& o, bool json) {
  encode(std::string_view(v), o, json);
}

/// @brief 编码 C 字符串；nullptr 在 JSON 下输出 null、文本下输出 (null)。
/// @param v 待编码的值；@param o 输出缓冲；@param json 是否为 JSON 格式。
inline void encode(const char* v, std::string& o, bool json) {
  if (!v) {
    o += json ? "null" : "(null)";
    return;
  }
  encode(std::string_view(v), o, json);
}

/// @brief 编码布尔值，输出 true / false（JSON 下也不加引号，因此无需区分格式）。
/// @param v 待编码的值；@param o 输出缓冲。
inline void encode(bool v, std::string& o, bool /*json*/) {
  o += v ? "true" : "false";
}

/// @brief 编码单个字符，按单字符字符串处理（JSON 下带引号）。
/// @param v 待编码的值；@param o 输出缓冲；@param json 是否为 JSON 格式。
inline void encode(char v, std::string& o, bool json) {
  encode(std::string(1, v), o, json);
}

/// @brief 编码整数（各宽度有符号 / 无符号），排除 bool 与字符类型。
///        数字在 JSON 下也不加引号，因此无需区分格式。
/// @tparam T 整数类型。
/// @param v 待编码的值；@param o 输出缓冲。
template <typename T,
          std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool> &&
                               !std::is_same_v<T, char> && !std::is_same_v<T, wchar_t> &&
                               !std::is_same_v<T, char16_t> && !std::is_same_v<T, char32_t>,
                           int> = 0>
void encode(T v, std::string& o, bool /*json*/) {
  o += std::to_string(v);
}

/// @brief 编码浮点数：%g 最短表示；nan / inf 在 JSON 下输出 null 以保证合法。
/// @param v 待编码的值；@param o 输出缓冲；@param json 是否为 JSON 格式。
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

/// @brief 编码 float，内部转为 double 处理。
/// @param v 待编码的值；@param o 输出缓冲；@param json 是否为 JSON 格式。
inline void encode(float v, std::string& o, bool json) {
  encode(static_cast<double>(v), o, json);
}

/// @brief 编码数组 / 容器：JSON 下 `[a,b,c]`，文本下 `[a, b, c]`。
/// @tparam T 元素类型。
/// @param v 待编码的容器；@param o 输出缓冲；@param json 是否为 JSON 格式。
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

/// @brief 编码 std::exception，只取 what()。
/// @param v 待编码的异常；@param o 输出缓冲；@param json 是否为 JSON 格式。
/// @note 必须按引用接收：按值传会把派生异常**切片**成 std::exception，
///       于是 what() 退化成基类的 "std::exception"，原始错误信息丢失。
inline void encode(const std::exception& v, std::string& o, bool json) {
  encode(v.what(), o, json);
}

/// @brief 编码时间点：ISO8601 时间（含毫秒）。定义在 field.cpp。
/// @param v 待编码的时间点；@param o 输出缓冲；@param json 是否为 JSON 格式。
void encode(std::chrono::system_clock::time_point v, std::string& o, bool json);

/// @brief 编码调用栈：文本折叠为单行，JSON 转义换行。定义在 stacktrace.cpp。
/// @param v 待编码的堆栈；@param o 输出缓冲；@param json 是否为 JSON 格式。
void encode(const StackTrace& v, std::string& o, bool json);

/// @brief 自定义类型的回退路径：具备 operator<< 的类型走流式输出
///        （文本原样，JSON 下当作字符串加引号）。
/// @tparam T 具备 operator<< 的类型。
/// @param v 待编码的值；@param o 输出缓冲；@param json 是否为 JSON 格式。
template <typename T, std::enable_if_t<!std::is_arithmetic_v<T> && !std::is_enum_v<T>, int> = 0,
          typename = decltype(std::declval<std::ostream&>() << std::declval<const T&>())>
void encode(const T& v, std::string& o, bool json) {
  std::ostringstream oss;
  oss << v;
  encode(oss.str(), o, json);
}

/// @brief 便捷入口：直接返回编码后的字符串。
/// @tparam T 任意可编码类型。
/// @param v 待编码的值。
/// @param json 是否为 JSON 格式。
/// @return 编码结果。
template <typename T>
std::string encode(T&& v, bool json) {
  std::string s;
  encode(std::forward<T>(v), s, json);
  return s;
}
/// @}

/// @brief 键值对的值：类型擦除容器。
///
/// 内部持有值的**拷贝**（独立于调用点）以及对应的编码函数，因此字段可以安全地随记录
/// 进入异步队列。用户一般不直接构造它 —— 用 KV() 即可。
class FieldValue {
 public:
  /// @brief 由值构造字段值，内部拷贝一份，生命周期独立于调用点。
  /// @tparam T 值的类型。
  /// @param v 源值。
  /// @return 持有该值拷贝的 FieldValue。
  /// @note 枚举没有编码重载：这里给出明确的 static_assert 提示，而不是让编译错误落在
  ///       encode 的重载决议上（那种报错是一大段模板候选列表，看不出真正原因）。
  template <class T>
  static FieldValue from(T&& v);

  /// @brief 把当前值按目标格式追加编码到 out。
  /// @param out 输出缓冲。
  /// @param json 是否为 JSON 格式（字符串加引号并转义）。
  void encode(std::string& out, bool json) const;

  FieldValue() = default;

  /// @brief 移动构造。
  /// @param o 源对象。
  FieldValue(FieldValue&& o) noexcept;
  /// @brief 移动赋值。
  /// @param o 源对象。
  /// @return 自身引用。
  FieldValue& operator=(FieldValue&& o) noexcept;
  /// @brief 拷贝构造，深拷贝持有的值。
  /// @param o 源对象。
  FieldValue(const FieldValue& o);
  /// @brief 拷贝赋值，深拷贝持有的值。
  /// @param o 源对象。
  /// @return 自身引用。
  FieldValue& operator=(const FieldValue& o);
  ~FieldValue();

 private:
  /// @brief 交换两份资源。
  /// @param o 交换对象。
  void swap(FieldValue& o) noexcept;

  void* obj_ = nullptr;                                             ///< 存储的值指针
  void (*encode_)(const void*, std::string&, bool json) = nullptr;  ///< 对应类型的编码器
  void* (*clone_)(const void*) = nullptr;                           ///< 对应类型的克隆函数
  void (*destroy_)(void*) = nullptr;                                ///< 对应类型的析构函数
};

/// @brief 一个结构化字段：字段名 + 类型擦除的值。
struct Field {
  std::string key;   ///< 字段名；空 key 会在入库前被跳过
  FieldValue value;  ///< 字段值（保留类型）
};

/// @brief 构造字段，key 为字符串字面量（编译期拦截空 key）。
///
/// @code
/// LOG_INFO("login", KV("user", name), KV("amount", 99.5));
/// LOG_INFO("oops", KV("", x));   // 编译错误
/// @endcode
///
/// @tparam N key 的数组长度，只用于编译期判空 —— 长度本身按 strlen 取，
///           因为传进来的也可能是一个运行期数组（如 `char buf[32]`）。
/// @tparam T 值的类型。
/// @param key 字段名（字符串字面量）。
/// @param v 字段值。
/// @return 构造好的字段。
template <std::size_t N, class T>
Field KV(const char (&key)[N], T&& v) {
  static_assert(N > 1, "KV key must not be empty");
  return Field{std::string(key), FieldValue::from(std::forward<T>(v))};
}

/// @brief 构造字段，key 为运行期字符串（如来自变量）。
///
/// 运行期无法在编译期校验，空 key 会在日志入库阶段被直接跳过。
///
/// @tparam T 值的类型。
/// @param key 字段名。
/// @param v 字段值。
/// @return 构造好的字段。
template <class T>
Field KV(std::string_view key, T&& v) {
  return Field{std::string(key), FieldValue::from(std::forward<T>(v))};
}

// 见类内声明处的文档
template <class T>
FieldValue FieldValue::from(T&& v) {
  using U = std::decay_t<T>;
  static_assert(!std::is_enum_v<U>,
                "枚举没有默认编码：请显式转换（如 static_cast<int>(v)），或为它重载 encode()");

  FieldValue fv;
  fv.obj_ = new U(std::forward<T>(v));  // 拷贝/移动进来，值独立于调用点
  // 用 if constexpr 而不是直接赋值：static_assert 只负责报告，不会阻止下面的 lambda
  // 被实例化，枚举下会再刷一屏 encode 重载决议的候选列表
  if constexpr (!std::is_enum_v<U>) {
    fv.encode_ = [](const void* p, std::string& out, bool json) {
      using ::logger::encode;  // 先把库的重载带进作用域（否则会先找到成员 FieldValue::encode）
      encode(*static_cast<const U*>(p), out, json);  // 非限定：ADL 才能找到用户命名空间里的重载
    };
  }
  fv.clone_ = [](const void* p) -> void* { return new U(*static_cast<const U*>(p)); };
  fv.destroy_ = [](void* p) { delete static_cast<U*>(p); };
  return fv;
}

/// @brief 追加字段；空 key 直接跳过。
/// @param fields 目标字段列表。
/// @param f 待追加的字段。
void append_field(std::vector<Field>& fields, Field f);

/// @brief 字段去重：同 key 后写覆盖（保留最后一个值），位置取首次出现。
/// @param fields 待去重的字段列表。
void dedup_fields(std::vector<Field>& fields);

/// @brief 可变参数拆分的递归终止（参数包为空）。
/// @return 空 tuple。
inline std::tuple<> split_fields(std::vector<Field>& /*fields*/) {
  return {};
}

/// @brief 拆分可变参数：Field 进 fields（保持顺序），其余进 tuple 作为位置参数（保持顺序）。
/// @tparam T 首个参数类型。
/// @tparam Rest 其余参数类型。
/// @param fields 字段收集器。
/// @param first 首个参数。
/// @param rest 其余参数。
/// @return 位置参数组成的 tuple（保持原有顺序）。
template <typename T, typename... Rest>
auto split_fields(std::vector<Field>& fields, T&& first, Rest&&... rest) {
  if constexpr (std::is_same_v<std::decay_t<T>, Field>) {
    append_field(fields, std::forward<T>(first));  // 空 key 跳过
    return split_fields(fields, std::forward<Rest>(rest)...);
  } else {
    auto tail = split_fields(fields, std::forward<Rest>(rest)...);
    return std::tuple_cat(std::forward_as_tuple(std::forward<T>(first)), std::move(tail));
  }
}

}  // namespace logger
