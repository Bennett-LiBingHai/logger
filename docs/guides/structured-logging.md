# 结构化日志指南

> 下文示例假定文件顶部有 `#include <logger/logger.h>`（统一入口，其余公共头都由它带进来）
> 和 `using namespace logger;`（库的公共符号都在 `logger::` 下，只有 `LOG_*` 宏在全局）。
> 示例里出现的 `#include <logger/xxx.h>` 只是为了指明那个能力定义在哪个头，
> 实际只需包含统一入口 `logger/logger.h`。


结构化日志 = 消息正文 + 一组有名字、有类型的字段。正文给人看，字段给机器查。
本库两个都输出：文本格式下字段跟在正文后面（`key=value`），JSON 格式下是独立成员。

## 两个入口

| 入口 | 形式 | 输出调试信息 |
|---|---|---|
| 宏 | `LOG_INFO("msg", KV(...))` | ✅ `[thread][file:line][func]` |
| 结构化 | `Logger::get_instance().info("msg", KV(...))` | ❌ 不输出 |

```cpp
// 宏：带调用点，便于定位
LOG_INFO("order created", KV("order_id", "ORD-1001"), KV("amount", 99.5));

// 结构化：不带文件行号，适合被封装进业务日志函数
Logger::get_instance().info("order created", KV("order_id", "ORD-1001"));
```

两者的字段行为完全一致，区别只在于是否带调用点。内部实现是 `msg.file == nullptr`
即视为结构化入口，格式化时省掉文件 / 行号 / 函数。

## 支持的类型

`KV("key", value)` 里的 `value` 会被**拷贝**一份存进字段（不是引用调用点的对象），
所以字段可以安全地进异步队列、活过调用点。类型支持见下表：

| 类型 | 文本 | JSON |
|---|---|---|
| `std::string` / `std::string_view` / `const char*` | 原样 | `"..."`，转义 `"` `\` 与控制字符 |
| `const char* = nullptr` | `(null)` | `null` |
| 整数（各宽度、有无符号） | 十进制 | 十进制 |
| 浮点（`float` / `double`） | `%.6g` | `%.6g`；nan / inf → `null` |
| `bool` | `true` / `false` | `true` / `false`（不加引号） |
| `char` | 单字符 | `"c"` |
| `std::vector<T>` | `[a, b, c]` | `[a,b,c]`，元素递归编码 |
| `std::chrono::system_clock::time_point` | ISO8601（含毫秒） | 带引号 |
| `std::exception` | `what()` | 带引号 |
| `StackTrace` | 折叠为单行 | 转义换行 |
| 带 `operator<<` 的自定义类型 | 流式输出 | 当字符串加引号 |

浮点用 `%.6g` 而不是 `std::to_string`：后者对 `1e20` 会输出一长串数字，`%.6g` 走科学计数法。
JSON 下 nan / inf 必须写成 `null` —— `NaN` 不是合法 JSON，写出去会让整行解析失败。

### 枚举不支持

枚举**没有**编码重载，直接传会编译失败，报错是一句人话：

```
field.h:256:23: error: static assertion failed:
  枚举没有默认编码：请显式转换（如 static_cast<int>(v)），或为它重载 encode()
```

```cpp
enum class Color { Red, Green };
LOG_INFO("paint", KV("color", Color::Red));                     // ✗ 编译错误
LOG_INFO("paint", KV("color", static_cast<int>(Color::Red)));   // ✓
```

这是刻意的：静默把枚举变成整数会丢语义，而「转成什么」只有调用方知道
（底层整型？`to_string` 后的名字？）。显式转换把选择留在调用点。

拦得住是**因为类型在编译期就定了** —— 值可以是运行期的（比如从网络反序列化来的），
但静态类型不变，`KV("color", c)` 里的 `c` 无论值从哪来，编译期都是 `Color`：

```cpp
Color c = from_network();
LOG_INFO("paint", KV("color", c));      // ✗ 一样编译失败
```

### 自定义类型

两条路，优先用第一条：

```cpp
// 1) 重载 encode：控制两种格式下的表示
struct Money { long cents; std::string currency; };

inline void encode(const Money& v, std::string& o, bool json) {
    if (json) { o += "{\"cents\": "; o += std::to_string(v.cents); o += "}"; }
    else      { o += std::to_string(v.cents) + v.currency; }
}

LOG_INFO("paid", KV("amount", Money{999, "CNY"}));
// 文本 → amount=999CNY    JSON → "amount": {"cents": 999}
```

```cpp
// 2) 只提供 operator<<：库回退到 ostringstream，JSON 下当字符串处理
struct Endpoint { std::string host; int port; };
std::ostream& operator<<(std::ostream& os, const Endpoint& e) { return os << e.host << ':' << e.port; }
```

`encode` 是文本与 JSON 的**共同入口**——同一个值在两种格式下只是序列化形式不同，
语义必须一致。重载 `encode` 就同时接入了两种格式，不需要分别适配 Formatter。

## 字段顺序与重复 key

字段按**调用顺序**输出。合并了多个来源（`with()` 预绑定、`ContextScope`、调用点 KV）
之后，同 key **后写覆盖**，但位置保留**首次出现**的位置：

```cpp
auto lg = Logger::get_instance().with(KV("env", "prod"), KV("svc", "api"));
{
    ContextScope ctx{KV("env", "staging")};     // 上下文里的 env 覆盖 with() 的
    lg.info("hi", KV("svc", "worker"));         // 调用点的 svc 覆盖 with() 的
    // env=staging svc=worker   ← 顺序仍是 env 先（首次出现的位置）
}
```

优先级：**调用点 KV > `ContextScope` > `with()` 预绑定**。

## 空 key

```cpp
LOG_INFO("oops", KV("", value));        // ✗ 编译错误：static_assert(N > 1)
LOG_INFO("oops", KV(name, value));      // name 是 std::string，运行期为空 → 静默跳过
```

字面量 key 在编译期拦截；来自变量的 key 无法编译期校验，入库前跳过该字段（不影响其它字段）。

> 有一处容易误解：`static_assert` 是**编译期**检查，它拦的是「字面量 `""`」，不是「运行期算出来是空」。
> 非常量数组（如 `char buf[32]`）走的是同一个重载，此时长度按 `strlen` 取 ——
> 运行期为空的数组会退化成空串，然后在入库时被跳过，不会变成一串 NUL 字段名。

## 字段命名规范

没有强制，但建议统一，避免同一个概念在不同模块叫不同名字：

```text
time  level  msg  service  version  host  pid  thread_id
request_id  trace_id  span_id  error  stacktrace
```

- 全小写 + 下划线（与 JSON 生态一致，字段可直接落 ES / ClickHouse 而不用改映射）
- `time` / `level` / `msg` 是保留名，框架自己输出，业务字段别用（会与框架成员撞名）
- 链路字段用 `trace_id` / `span_id`，别自创 `traceId` —— 见 [Trace 集成指南](trace.md)

## 转义规则

字段值的转义由格式决定，调用方不用管：

| 格式 | 规则 |
|---|---|
| 文本 | `\` → `\\`；`\n` `\r` `\t` `\b` `\f` → 同名转义序列（两个字符）；其余控制字符与 `0x7F` → `\xHH` |
| JSON | 按 JSON 规范：`"` `\` `\n` `\r` `\t` `\b` `\f` 转义，其余控制字符 → `\u00XX` |

文本的转义是**可逆**的（反斜杠本身也转义），因此不会出现「值里带 `\n` 就伪造出一条新日志」。
详见 [故障排查](troubleshooting.md) 里的注入章节。

## 相关

- [上下文指南](context.md) — 自动附加字段，不用层层传参
- [脱敏指南](masking.md) — 字段级敏感信息处理
- [milestone §三](../milestone.md) — 字段设计决策
