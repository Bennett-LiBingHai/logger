# 兼容性说明

这份文档回答一个问题：**升级这个库时，什么可能会坏？**

对应 M7 §7「版本和兼容性」。结论先行：

| | 承诺 |
|---|---|
| API | 公共头文件里的声明与语义，**自 v1.0.0 起**按语义化版本承诺 |
| ABI | **只承诺同编译器 + 同标准库 + 同标准下的稳定**，不承诺跨标准库 |
| 稳定性豁免 | `detail/` 目录、`src/`、日志的文本排版、性能数字 |
| 当前版本 | **v1.0.0**，API 已稳定，兼容性承诺自本版本起生效 |

## 1. 公共 API 的边界

**公共面**就是这些头文件，它们的内容即接口承诺：

```text
include/logger/
├── logger.h          logger 单例、LOG_* 宏、with()
├── level.h           级别枚举
├── config.h          配置、LogStats、脱敏规则
├── field.h           encode 扩展点、Field / FieldValue、KV
├── literals.h        容量字面量（10_mb 等）
├── version.h         版本号（LOG_VERSION / LOG_VERSION_MAJOR 等）
├── context.h         ContextScope
├── stacktrace.h      StackTrace
├── trace.h           TraceContext 与 traceparent 编解码
├── crash_handler.h   install/uninstall_crash_handler
├── sink.h            LogSink 抽象 + SinkInput
└── sink/{console_sink,file_sink}.h
```

**不承诺**的部分：

- `include/logger/detail/` 与 `src/` —— 随时可改，任何符号都可能消失或改签名
- 日志的**文本排版**（时间戳/级别/字段的排列、分隔符）—— 给人看的，不承诺机器可解析
- 日志**文件名规则**（`YYYY_MM_DD_HH.log` 等）—— 实际是稳定的（采集器依赖它），但改它只算 MINOR
- 性能数字（[性能报告](performance.md) 里的每个数都会随编译器/硬件变）
- 崩溃日志的文本排版

## 2. 什么算破坏性变更

| 算 | 不算 |
|---|---|
| 删 / 改公共头文件里的函数签名、返回类型、参数默认值 | 新增函数、重载、头文件 |
| 改语义（字段覆盖顺序、去重键、脱敏命中规则） | 性能优化、内部重构 |
| 改**默认值**（如 `log_level`、`dedup_window_ms`、`stacktrace`） | 新增配置项（带默认值，不写就是旧行为） |
| 删 / 改 `LogConfig`、`FileSinkConfig`、`LogStats` 的字段名或类型 | 新增 `LogStats` 字段（只读快照，加字段不影响读取） |
| 删枚举值，或改其含义 | 新增枚举值（调用方 `switch` 若无 `default` 会收到编译警告） |
| 改框架产出的 JSON / 文本字段名 | 新增框架字段（不重名时） |
| 改宏的名字、参数个数、展开语义 | 新增宏 |

改默认值这条容易被忽略但影响最大：升级后脱敏规则、堆栈采集、去重窗口都可能直接变，而源码一个字没动。

## 3. ABI

**承诺范围**：同一个编译器大版本（如 gcc 13）、同一个标准库（libstdc++）、同一个 C++ 标准
（≥ C++17）、同一组 ABI 相关编译选项。这个范围内的二进制可以混链。

**明确不承诺**：

| 场景 | 为什么不行 |
|---|---|
| libstdc++ ↔ libc++ | `std::string` / `std::vector` 布局不同 |
| gcc ↔ clang | 名称修饰、异常表、虚表布局都可能不同 |
| `_GLIBCXX_USE_CXX11_ABI=0` ↔ `=1` | `std::string` 是两种类型 |
| 带/不带 `-D_GLIBCXX_ASSERTIONS`、`-fno-exceptions` | 头文件里的内联代码被编译进调用方 |

接口上直接暴露了标准库类型，所以这条线躲不掉：

```cpp
class LogSink {                                  // 纯虚类，vtable 布局随编译器
  virtual bool log(const SinkInput& input) = 0;  // SinkInput 内含 std::string
  virtual void flush() = 0;
};

struct LogConfig {
  using MaskerFn = std::function<std::string(const std::string&, const std::string&)>;
  MaskerFn sensitive_field_masker;               // std::function 跨编译器不兼容
};
```

**实践建议**：本库是静态库，跟应用一起编译即可，不要跨编译器混链。
另外 `-rdynamic` 由 `INTERFACE` 传递，最终可执行文件必须带上（否则堆栈符号化退化成模块名 + 偏移）。

## 4. 宏与命名冲突

宏都带 `LOG_` 前缀，与业务宏冲突的概率很低：

```text
LOG_TRACE  LOG_DEBUG  LOG_INFO  LOG_WARN  LOG_ERROR  LOG_FATAL  LOG_EXCEPTION
```

**`KV` 没有前缀** —— 它是函数模板而不是宏（类型安全、能编译期拦空 key），所以不进宏命名空间。

真正需要留意的是**全局命名空间里的非宏符号**。库为了可用性把它们放在全局：

| 符号 | 为什么在全局 |
|---|---|
| `encode(...)` 重载集 | 自定义类型的扩展点，必须在全局才能被库内的调用找到（见下） |
| `KV(...)` | 调用点天天要写，加命名空间太啰嗦 |
| `filename_of` | `LOG_*` 宏展开时用 |
| `install_crash_handler` / `uninstall_crash_handler` | 无前缀的顶层功能 |
| `is_sensitive_key` / `default_sensitive_field_masker` | 自定义 masker 要复用关键词表 |
| `append_field` / `dedup_fields` / `split_fields` | `KV` 的配套，模板里要用 |

冲突时的处理：宏可以 `#undef`；函数可以用 `::encode` 显式限定，或干脆只用 `Logger` 成员函数不用宏。
字号量后缀（`10_mb`）在 `namespace logger::literals` 里，**需要 `using` 才生效**，不污染全局。

### 自定义类型的扩展点有个限制

`encode` 是自定义类型的接入方式，但库内是**限定调用** `::encode(...)` —— 限定调用不触发 ADL，
所以重载必须定义在**全局命名空间**：

```cpp
// ✓ 可以：全局命名空间
inline void encode(const Money& v, std::string& o, bool json) { ... }

// ✗ 不行：自己的命名空间（ADL 被 :: 关掉，找不到）
namespace my { inline void encode(const Money&, std::string&, bool) { ... } }
```

绕不开时用第二条路：只提供 `operator<<`，库会退到 `ostringstream`（JSON 下当字符串处理）。
这条限制登记在 [破坏性变更清单](breaking-changes.md) 的待改进项里。

## 5. 配置兼容性

`LogConfig` 是纯值结构，`set_config()` 是**整份替换**，不是逐字段合并：

```cpp
LogConfig cfg;                 // 全部字段 = 默认值
cfg.log_level = LogLevel::INFO;
Logger::get_instance().set_config(cfg);   // 其余字段被重置为默认值
```

这带来一个升级陷阱：**不要把配置存成结构体长期持有，再改一个字段就 set 回去**。
新版本一旦加了字段，旧结构体里那些字段是默认值，会把当前配置覆盖掉。正确写法是就地取快照：

```cpp
LogConfig cfg = Logger::get_instance().get_config();
cfg.log_level = LogLevel::INFO;
Logger::get_instance().set_config(cfg);
```

只改级别的话用 `set_level()`，它不碰其它字段。

**不做配置文件解析**：库不读 JSON/YAML/INI，配置由应用自己产生，所以没有"配置格式兼容"这回事。
如果你把 `LogConfig` 序列化下来，新增字段的默认值策略由你决定。

## 6. JSON 字段兼容性

框架占用的成员名（**保留**）：

```text
time  level  msg  repeated  thread_id  file  line  func
```

- `time` / `level` / `msg` 在前，`msg` 之后是**用户字段**（按调用顺序），最后是调试信息
- `repeated` 仅在聚合去重折叠了重复消息时出现，值是折叠次数
- 新增框架成员属 MINOR（不与用户字段重名时）；改名或删除属 MAJOR

### ⚠️ 用户字段不能与保留名重名

库目前**不会拦截**重名，而 JSON 输出会出现**重复键**：

```cpp
Logger::get_instance().info("hi", KV("level", "boom"));
```

```json
{"time": "...", "level": "info", "msg": "hi", "level": "boom"}
```

RFC 8259 里对象名「SHOULD be unique」，解析器行为不一致：Python 的 `json` 取最后一个
（于是级别变成 `boom`），有的库直接报错。对按 `level` 做路由/告警的采集链来说这是**静默的错误数据**。

请把上面 8 个名字当成保留字。库要不要主动丢弃或改名，见
[破坏性变更清单](breaking-changes.md) 的待改进项。

### 文本格式不承诺可解析

`key=value` 的文本输出是给人看的。要机器消费请用 JSON（`cfg.format = LogFormat::JSON`）。
文本里的转义是**可逆**的，但没有承诺过"用什么规则解析"，别在它上面写正则。

## 7. 废弃流程

1. 先标 `[[deprecated("用 xxx 替代")]]`，**至少保留一个小版本**
2. 下一个 MAJOR 版本才删除
3. 同一时间只积累有限的废弃项，避免长期维护两套

宏没法标 `[[deprecated]]`（编译器属性作用在声明上），所以宏的废弃只能靠 CHANGELOG 与文档说明。

**当前没有任何废弃项。**

## 8. 版本号与 Release Note

版本号遵循[语义化版本](https://semver.org/lang/zh-CN/)：

```text
MAJOR.MINOR.PATCH
  │     │     └─ 向后兼容的修复
  │     └─────── 向后兼容的新增
  └───────────── 破坏性变更
```

**自 v1.0.0 起承诺兼容**：破坏性变更只在 MAJOR 版本发生，且必须给出迁移方法。
`0.x` 期间（v0.1.0 – v0.6.0）的破坏性变更已记进 [CHANGELOG](../CHANGELOG.md) 与
[破坏性变更清单](breaking-changes.md)。

### 在代码里取版本号

| 宏 | 值 | 用途 |
|---|---|---|
| `LOG_VERSION` | `"1.0.0"` | 打日志 / 版本输出 |
| `LOG_VERSION_MAJOR` / `MINOR` / `PATCH` | `1` / `0` / `0` | 分量比较 |
| `LOG_VERSION_CODE` | `10000` | `#if` 比较：`#if LOG_VERSION_CODE >= 10100` |

```cpp
#include <logger/version.h>

LOG_INFO("logger {} starting", LOG_VERSION);
```

三个分量各写一份值，与 `CMakeLists.txt` 的 `project(VERSION)` 是两处维护 ——
`test_version` 里有一条用例专门比对二者，漂移会直接测失败。

新增版本宏或改其语义属 MAJOR；新增分量（如带构建号）属 MINOR。

### Release Note 规范

[CHANGELOG.md](../CHANGELOG.md) 遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)：

- 分类：`Added` / `Changed` / `Deprecated` / `Removed` / `Fixed` / `Security`
- **每个破坏性变更必须写「怎么迁移」**，只写"改了 X"没有价值
- 破坏性变更同时登记到 `docs/breaking-changes.md`（那里按「现象 → 原因 → 迁移」组织）
- 日期用 `YYYY-MM-DD`，倒序排列（最新在最上面）
- 未发布的改动写在 `## [Unreleased]` 下

## 相关

- [CHANGELOG.md](../CHANGELOG.md) — 逐版本变更记录
- [破坏性变更清单](breaking-changes.md) — 已发生的破坏性变更与迁移方法
- [milestone.md](milestone.md) — 设计决策与路线图
