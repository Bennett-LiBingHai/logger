# logger {#mainpage}

C++17 结构化日志库。无第三方运行时依赖，静态库接入。

**当前版本 v1.0.0** —— API 已稳定，自本版本起按语义化版本承诺兼容性。

> 这里是 API 参考。指南、决策记录与性能数据在仓库里，见文末链接。

## 快速开始

```cpp
#include <logger/logger.h>   // 统一入口：其余公共头都由它带进来
#include <memory>

using namespace logger;      // 库的公共符号都在 logger:: 下

int main() {
    // Error 及以上 → stderr，其余 → stdout
    Logger::get_instance().add_sink(std::make_shared<ConsoleSink>());

    LOG_INFO("user {} login", 1001);                       // {} 位置参数（非 printf 风格）
    Logger::get_instance().info("order created",           // 结构化字段
                                KV("order_id", "ORD-1001"),
                                KV("amount", 99.5));

    Logger::get_instance().flush_all();
}
```

只有 `LOG_*` 宏在全局命名空间（宏没法放进命名空间，宏内部用的是全限定名，
所以在任何命名空间里都能直接调用）。

## 能力一览

| 方面 | 内容 |
|---|---|
| 级别 | `TRACE` / `DEBUG` / `INFO` / `WARN` / `ERROR` / `FATAL` / `OFF`，入口处无锁早退 |
| 格式 | 文本 / JSON 两种，由 `LogConfig::format` 切换 |
| 字段 | `KV("key", value)`，保类型；同一 key 后写覆盖 |
| 上下文 | `ContextScope`（thread_local + RAII）与 `Logger::with()` 预绑定 |
| 错误 | `LOG_EXCEPTION` 自动展开 `error` / `error_type` / `error_chain`；`StackTrace` 采集调用栈 |
| 链路 | `TraceContext` 编解码 W3C traceparent，不绑定追踪后端 |
| 输出 | 控制台（按级别分流）、文件（按日期 / 大小轮转）、自定义 `LogSink` |
| 模式 | 同步 / 异步（惰性后台线程、四种队列满策略、优雅关闭） |
| 治理 | 敏感字段脱敏、注入防护（单行不变式）、三层长度预算、聚合去重 |
| 可观测 | `Logger::stats()`：写入 / 丢弃 / 队列水位 / 写出延迟 / 脱敏数 / 去重数 |
| 崩溃 | `install_crash_handler()` 记录精确现场并保留 core dump 语义 |
| 版本 | `LOG_VERSION` / `LOG_VERSION_CODE` 等宏 |

## 怎么读这份文档

- **类**：左侧 `Classes` —— `Logger` 是入口，`LogConfig` 是配置，`LogSink` 是输出扩展点
- **文件**：`Files` —— 每个公共头顶部都有该文件的说明
- **宏**：`File Members` —— `LOG_*` 与 `LOG_VERSION*`（其余符号都在 `logger::` 命名空间里，
  不在文件级）

设计决策、实现理由与「为什么不做某件事」都写在头文件的注释里，跟着类走。

## 更多文档

仓库内（GitHub）：

- [README](https://github.com/Bennett-LiBingHai/logger/blob/main/README.md) — 快速开始、配置项、构建与测试
- [docs/guides/](https://github.com/Bennett-LiBingHai/logger/tree/main/docs/guides) — 结构化日志、上下文、错误与堆栈、Trace、脱敏、同步与异步、文件轮转、故障排查
- [兼容性说明](https://github.com/Bennett-LiBingHai/logger/blob/main/docs/compatibility.md) — 承诺范围、ABI 边界、废弃流程
- [性能报告](https://github.com/Bennett-LiBingHai/logger/blob/main/docs/performance.md) — 16 个场景的实测数据与口径说明
- [CHANGELOG](https://github.com/Bennett-LiBingHai/logger/blob/main/CHANGELOG.md) — 逐版本变更
- [milestone.md](https://github.com/Bennett-LiBingHai/logger/blob/main/docs/milestone.md) — 设计与路线图

## 构建要求

- CMake ≥ 3.16，支持 C++17 的编译器（gcc / clang）
- `libdl`（`dladdr` 符号化用；glibc ≥ 2.34 已并入 libc）
- 测试需要 GoogleTest，基准需要 Google Benchmark（都是可选依赖）
