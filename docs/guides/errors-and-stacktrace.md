# 错误与堆栈指南

> 下文示例假定文件顶部有 `#include <logger/logger.h>`（统一入口，其余公共头都由它带进来）
> 和 `using namespace logger;`（库的公共符号都在 `logger::` 下，只有 `LOG_*` 宏在全局）。
> 示例里出现的 `#include <logger/xxx.h>` 只是为了指明那个能力定义在哪个头，
> 实际只需包含统一入口 `logger/logger.h`。


## 记录异常

手写 `catch` + 拼 `e.what()` 有几个常见问题：忘了带类型名；嵌套异常只看到最外层；
不同人写法不一致，查询时字段名对不上。`LOG_EXCEPTION` 把这些固定下来：

```cpp
#include <logger/logger.h>

try {
    db.query(sql);
} catch (const std::exception& e) {
    LOG_EXCEPTION("db query failed", e, KV("retry", 3), KV("sql", sql));
}
```

固定产生三个字段（额外 `KV` 照常附加）：

| 字段 | 内容 |
|---|---|
| `error` | 最内层异常的 `what()`，即根因 |
| `error_type` | 可读类型名，经 `__cxa_demangle` |
| `error_chain` | 完整 caused-by 链，**仅在存在嵌套时输出** |

`LOG_EXCEPTION` 的级别固定为 `ERROR`。要换级别用 `log_exception()`：

```cpp
Logger::get_instance().log_exception(LogLevel::FATAL, ..., "致命", e);
```

### 嵌套异常

`std::throw_with_nested` 抛出的实际类型是实现内部的包装类型
（libstdc++ 是 `std::_Nested_exception<T>`，libc++ 是 `std::__nested_exception<T>`）。
直接 demangle 出来的名字是模板噪音，对排查没有帮助，所以库会剥掉这层包装：

```cpp
try {
    try {
        connect();
    } catch (const std::exception& e) {
        std::throw_with_nested(std::runtime_error("db unavailable"));
    }
} catch (const std::exception& e) {
    LOG_EXCEPTION("query failed", e);
}
```

```text
error=runtime_error: db unavailable
error_type=std::runtime_error
error_chain=std::runtime_error: db unavailable\n  caused by: NetworkError: connection refused
```

（`error_chain` 里的换行在实际输出中是转义过的 —— 见下面的「单行不变式」。）

`error` / `error_type` 取**最内层**（根因），`error_chain` 从外层到内层排。
展开用标准机制 `std::rethrow_if_nested`，不依赖具体实现。

### 跨线程传递

异常对象不能跨线程移动，只能传 `std::exception_ptr`：

```cpp
std::exception_ptr ep;
auto fut = pool.submit([&] {
    try { work(); } catch (...) { ep = std::current_exception(); }
});
fut.wait();
if (ep)
    Logger::get_instance().exception("worker failed", ep);
```

`exception_ptr` 为空时记为 `<no exception>` 占位，不会崩。

### 结构化入口

不带文件行号时用 `Logger::exception`（级别固定 `ERROR`）：

```cpp
Logger::get_instance().exception("worker failed", e);
```

### 单行不变式

`error_chain` 天然是多行的（`\n  caused by: `），但**日志统一单行** —— 转义在渲染阶段做，
所以文本输出里是字面的 `\n` 两个字符，JSON 里是 `\n` 转义序列。两种格式下都能还原出原始
多行结构，但一条记录永远只占一行。理由见 [故障排查](troubleshooting.md#日志注入)。

## 调用栈

### 采集与符号化是分开的

```cpp
#include <logger/stacktrace.h>

LOG_ERROR("query failed", KV("stacktrace", StackTrace::capture()));
```

- `capture()` 只调 `backtrace()` 拿一串返回地址 —— 快、不分配，可以在热路径调
- `str()` 才做 `dladdr` + demangle，结果缓存，同一条记录只算一次

分开的理由：采集是每条日志都付的，符号化只在真的要输出时付。异步模式下采集在业务线程、
符号化在后台线程。

### 参数

```cpp
StackTrace::capture(std::size_t skip = 1, std::size_t depth = 10, std::size_t max_length = 512);
```

| 参数 | 含义 |
|---|---|
| `skip` | 从栈顶丢弃的帧数（含 `capture` 自身），默认 1 |
| `depth` | 最多保留多少帧 |
| `max_length` | 渲染结果的字节上限 |

`max_length` 超限时**按帧丢弃**（不是字节硬切），并在末尾补 `... (+N frames)`，
所以栈文本永远是完整的帧、可读的。

> `skip` 不做「过滤掉 Logger 自己的帧」这类启发式。之前试过按函数名过滤，在 `-O2` 下
> 会把 `main` 一起吞掉（内联后帧名对不上），比不过滤更难解释。统一 `skip = 1`，
> 让第一个业务帧之后的调用链完整呈现。

### 自动采集

`FATAL` **默认自动附上调用栈**：

```cpp
LOG_FATAL("invariant broken");   // 自动带 stacktrace 字段
```

| `config.stacktrace` | 行为 |
|---|---|
| `StackTraceMode::FATAL`（默认） | 仅 `FATAL` 自动采集 —— 致命错误通常只发生一次，值得付采集开销 |
| `StackTraceMode::OFF` | 完全关闭，**显式请求也不生效**（生产排障时可作全局开关） |
| `StackTraceMode::ALWAYS` | 所有级别都采（仅调试用，开销大） |

帧数与字节上限由 `config.stacktrace_depth`（默认 10）和 `config.max_stacktrace_length`
（默认 512）控制。

### 符号能否解出

自动采集的帧调 `dladdr` 符号化，需要可执行文件带**动态符号表**。CMake 里已经给目标加了
`-rdynamic`（通过 `INTERFACE` 传递到最终可执行文件）：

```cmake
target_link_options(logger INTERFACE -rdynamic)
```

`dladdr` 只能看到动态符号表里的符号，所以：

- 本可执行文件 / 动态库的**非 static 函数**：能解出名字
- `static` 函数、被内联掉的函数：解不出名字，退化成模块名 + 偏移
- 链接时加了 `-s`（strip）或用了 `-fvisibility=hidden`：解不出名字

解不出名字不代表栈没用 —— 模块 + 偏移仍能离线用 `addr2line` 拿到行号（见下）。

### 渲染格式

每帧一行，形如：

```text
#0 OrderService::submit (./order-api+0x1a2c)
#1 main (./order-api+0x8f10)
```

`+0x...` 是**相对该符号起点**的偏移（`dladdr` 的 `dli_saddr`），用来看出「函数里的哪个位置」；
它不等于模块内偏移，不能直接喂给 `addr2line`。要精确到行号，用崩溃处理器的输出 —— 它打印的
偏移已经减掉 load bias，正是 `addr2line` 需要的链接期地址（PIE / 非 PIE 一视同仁）：

```bash
addr2line -f -C -e ./order-api 0x1a2c
```

## 相关

- [故障排查](troubleshooting.md) — 崩溃日志怎么读
- [crash_handler.h](../../include/logger/crash_handler.h) — 崩溃现场捕获（signal handler
  里不能分配内存，所以是独立于 Logger 的另一套实现）
