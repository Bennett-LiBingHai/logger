# Logger

一个 C++17 的结构化日志库。无第三方运行时依赖，静态库接入，公共 API 就是 [include/logger/](include/logger/) 下的头文件。

当前版本：**v1.0.0**（M1–M7 全部完成）。API 已稳定，自本版本起按语义化版本承诺兼容性
（范围见[兼容性说明](docs/compatibility.md)）。完整路线见 [docs/milestone.md](docs/milestone.md)。

## 特性

**基础**

- 七种级别：`TRACE / DEBUG / INFO / WARN / ERROR / FATAL`，外加 `OFF` 关闭全部
- 宏接口 `LOG_TRACE` … `LOG_FATAL`，`{}` 位置参数格式化（自研，不用 printf、不用 iostream）
- 超长日志自动截断

**结构化**

- 字段 `KV("key", value)`：文本输出 `key=value`，JSON 输出带类型成员
- 字段类型：字符串、各宽度整数、浮点（含 nan/inf）、布尔、`char`、`std::vector`、时间点、带 `operator<<` 的自定义类型
- 同一 key 后写覆盖；空 key 字面量编译期拦截、动态 key 运行期跳过
- 预绑定字段 `with(KV(...))`，共享 sinks / config，返回子 `Logger`

**输出**

- 文本 / JSON 两种格式，`config.format` 切换
- 控制台按级别分流：`Error` 及以上 → stderr，其余 → stdout
- 文件输出 + 按日期 / 大小轮转，可限制保留文件数；目录缺失自动创建，文件被外部删除后自动重开
- 写失败策略（降级 stderr / 丢弃并计数），失败不崩溃
- 自定义 Sink：继承 `LogSink` 即可接入任意后端

**性能与并发**

- 同步 / 异步两种模式（异步惰性启动后台线程，业务线程入队即返回）
- 队列满策略：`Block` / `DropNewest` / `DropOldest` / `DropDebug`
- 优雅关闭：`close()` / 析构 等待队列清空并刷盘
- 全程无数据竞争（TSan 可验证）

**可观测（M5）**

- `ContextScope`：作用域上下文，进入作用域后本线程的日志自动带字段，RAII 出栈
- `LOG_EXCEPTION`：异常自动展开为 `error` / `error_type` / `error_chain` 三个字段
- `StackTrace`：调用栈采集与符号化分离
- `TraceContext`：W3C traceparent 编解码，链路字段跨服务接续

**治理与安全（M6）**

- 敏感字段脱敏（内置规则 + 自定义 masker）
- 日志注入防护：统一单行不变式，换行 / 控制字符一律转义
- 三层长度预算：消息、单字段、整条记录
- 聚合去重：同一处代码刷屏折叠为「首条 + 重复次数」
- 自身指标 `Logger::stats()`：写入 / 丢弃 / 队列峰值 / 写出延迟 / 脱敏数 / 去重数
- 运行期改级 `set_level()`
- 崩溃现场捕获：精确到故障地址与出错指令，保留 core dump 语义

## 依赖

- CMake ≥ 3.16
- 支持 C++17 的编译器（gcc / clang）
- `libdl`（`dladdr` 符号化用；glibc ≥ 2.34 已并入 libc，无需手动链接）
- 测试依赖 GoogleTest（可选，`BUILD_TESTS=ON` 时需要）

## 构建

```bash
cmake -S . -B build
cmake --build build
```

## 开发工具

代码格式化统一用 **clang-format 18**（配置见 [.clang-format](.clang-format)）：

```bash
sudo apt install clang-format-18     # 按大版本安装（Ubuntu 24.04 装 18.1.x）
clang-format-18 --version            # 确认输出 18.x
clang-format-18 -i <file>            # 格式化单个文件
```

> 精确锁到补丁版本：`sudo apt install clang-format-18=1:18.1.3-1ubuntu1`。
> 不同大版本的 clang-format 结果可能略有差异，升级后需全量重跑一次并统一提交。

静态检查用 **clang-tidy 18**（规则见 [.clang-tidy](.clang-tidy)）：

```bash
sudo apt install clang-tidy-18
cmake -S . -B build                  # 会生成 compile_commands.json
clang-tidy-18 -p build --quiet src/*.cpp src/detail/*.cpp \
    src/detail/formatter/*.cpp src/sink/*.cpp
```

检查集刻意收窄（`bugprone-*` / `clang-analyzer-*` / `performance-*` / `modernize-use-override`），
**保持零告警**——开了就修到零，否则真问题会淹在噪音里。头文件里的模板也会被扫到
（`HeaderFilterRegex`），CI 里 warning 即失败。

## 快速开始

```cpp
#include <logger/logger.h>
#include <logger/sink/console_sink.h>
#include <memory>

int main() {
    // Error 及以上 → stderr，其余 → stdout
    Logger::get_instance().add_sink(std::make_shared<ConsoleSink>());

    // {} 位置参数格式化
    LOG_INFO("user {} login", 1001);
    LOG_ERROR("connect failed: {}", "timeout");

    // 结构化字段（文本格式输出 key=value）
    Logger::get_instance().info("order created", KV("order_id", "ORD-1001"),
                                KV("amount", 99.5));

    // 切换 JSON 输出
    LogConfig cfg;
    cfg.format = LogFormat::JSON;
    Logger::get_instance().set_config(cfg);
    Logger::get_instance().info("payment ok", KV("trace_id", "abc123"));

    Logger::get_instance().flush_all();
    return 0;
}
```

完整可运行示例在 [examples/](examples/)，构建后直接跑：

```bash
cmake --build build
./build/logger_example          # 控制台 + 结构化字段 + JSON 切换
./build/example_file_logging    # 文件输出与按大小/日期轮转
./build/example_async_logging   # 异步写入 + 自身指标
./build/example_trace_logging   # traceparent 接续与传播
./build/example_masking         # 默认脱敏与自定义脱敏
./build/example_dynamic_level   # 运行期调整级别
```

## 日志级别

| 级别 | 说明 |
|---|---|
| `TRACE` | 极细粒度的执行过程 |
| `DEBUG` | 开发调试信息 |
| `INFO` | 正常业务流程 |
| `WARN` | 可恢复异常或潜在风险 |
| `ERROR` | 当前操作失败 |
| `FATAL` | 致命错误 |
| `OFF` | 关闭所有日志输出 |

低于 `config.log_level` 的日志会被忽略（在入口处尽早过滤，`OFF` 时开销接近一次函数调用）。

## 结构化字段

```cpp
// 文本：order created order_id=ORD-1001 amount=99.5
Logger::get_instance().info("order created", KV("order_id", "ORD-1001"),
                            KV("amount", 99.5));

// 预绑定字段（共享 sinks/config，返回子 Logger）
auto serviceLogger = Logger::get_instance().with(
    KV("service", "order-service"),
    KV("version", "1.2.0"),
);
serviceLogger.info("server started");  // 自动带 service / version
```

字段按调用顺序输出，同 key 后写覆盖。空 key：字面量 key 由编译期 `static_assert` 拦截，
运行时动态 key 直接跳过。完整规则（类型支持、命名规范、非法字段、转义）见
[结构化日志指南](docs/guides/structured-logging.md)。

## 上下文与链路

作用域内打的所有日志自动带字段，不用把 `Logger` 沿调用链传下去：

```cpp
{
    ContextScope ctx{KV("request_id", req.id), KV("user_id", req.user)};
    LOG_INFO("handling request");   // 自动带 request_id / user_id
}   // 出作用域自动出栈，支持嵌套（内层覆盖外层同名 key）
```

链路接续（W3C traceparent，不绑定任何追踪后端）：

```cpp
TraceContext tc;
if (!parse_traceparent(req.header("traceparent"), tc))
    tc = generate_trace();                            // 没有上游就自己起一条
ContextScope ctx{KV("trace_id", tc.trace_id), KV("span_id", tc.span_id)};
LOG_INFO("handling request");
http_client.set_header("traceparent", make_traceparent(tc));   // 传给下游
```

字段优先级：**调用点的显式 `KV` > `ContextScope` > `with()` 预绑定**。
`ContextScope` 是 `thread_local`，不跨线程；子线程需要延续时用 `Logger::with(...)` 显式传值。

细节见 [上下文指南](docs/guides/context.md) 和 [Trace 集成指南](docs/guides/trace.md)。

## 错误与异常

`LOG_EXCEPTION` 自动展开异常信息，不用手写 `e.what()`：

```cpp
try {
    db.query(sql);
} catch (const std::exception& e) {
    LOG_EXCEPTION("db query failed", e, KV("retry", 3), KV("sql", sql));
}
```

自动产生三个字段：

| 字段 | 内容 |
|---|---|
| `error` | 最内层异常的 `what()`，即根因 |
| `error_type` | 可读类型名（经 `__cxa_demangle`） |
| `error_chain` | 完整 caused-by 链，仅在存在嵌套时输出 |

嵌套异常剥掉 `std::_Nested_exception<T>` 这层实现细节，只留业务类型名。
跨线程传递用 `std::exception_ptr` 重载（`Logger::exception(msg, eptr)`）。

需要调用栈时显式传，采集与符号化是分开的（`capture()` 只存地址，`str()` 才 demangle）：

```cpp
#include <logger/stacktrace.h>

LOG_ERROR("query failed", KV("stacktrace", StackTrace::capture()));
```

`FATAL` **默认自动附上调用栈**（`config.stacktrace = StackTraceMode::FATAL`）——致命错误
通常只发生一次，值得付采集开销。`OFF` 连显式请求也不生效（生产排障时可作全局开关），
`ALWAYS` 所有级别都采（仅调试用，开销大）。栈文本里换行折叠为 `|`，仍遵守单行不变式。

## 崩溃现场捕获

signal handler 里不能 `malloc`、不能用 `std::string`，`Logger` 的锁也可能正被崩溃线程持有，
所以崩溃记录是独立于 Logger 的一套实现：

```cpp
#include <logger/crash_handler.h>

install_crash_handler("/var/log/app/crash.log");   // 空串 → 写 stderr
```

进程收到 `SIGSEGV / SIGABRT / SIGBUS / SIGFPE / SIGILL` 时写下：pid / tid、信号名与编号、
`si_code`、故障地址、出错指令指针（PC / RSP / RBP）、image base、栈回溯原始地址。

- **不做符号化**：输出里的偏移量 = 运行时地址 − 主程序 load bias，正是 `addr2line` 需要的链接期地址，PIE 与非 PIE 一视同仁
- 写完现场后恢复默认处理并**重抛信号，保留 core dump 语义**
- 用独立信号栈（`sigaltstack`），栈溢出导致的崩溃也能记录
- 某信号上**已有非默认 handler**（ASan / TSan、JVM 等）时**不接管**并在 stderr 提示——JVM 用 SIGSEGV 做隐式空指针检查，抢过来会破坏正常流程
- `uninstall_crash_handler()` 精确还原接管前的 handler，不会动其它库装的

## 安全与治理

### 脱敏

```cpp
LogConfig cfg;
cfg.enable_sensitive_field_mask = true;     // 默认 false
// cfg.sensitive_field_masker = my_masker;  // 可选：自定义 (key, value) -> value
Logger::get_instance().set_config(cfg);
LOG_INFO("login", KV("password", "hunter2"), KV("user", "alice"));
// → login password=*** user=alice
```

内置规则是对 key 的**精确匹配**（区分大小写），命中即整体替换为 `***`：

```
password  passwd  token  access_token  refresh_token  secret
authorization  cookie  private_key  credit_card  id_card
```

masker 拿到的是**未编码的原始值**，自定义实现不会受转义影响；返回值与原值相同时不计入
`masked_fields`。内置规则不够用时换掉 `sensitive_field_masker` 即可，见 [脱敏指南](docs/guides/masking.md)。

### 注入防护

用户数据里带 `\n` 会伪造出一条假日志（日志注入）。本库的不变式是：**一条记录永远只占一行**。

- 换行、回车、`\t`、`\0`、`0x7F` 及其它控制字符一律转义（`\n` 写成 `\n` 两个字符）
- 反斜杠本身也转义，因此转换**可逆**，能还原出原始输入
- 每条记录末尾补 `\n`，所以每条日志恰好一行，不受输入影响

### 长度限制

| 配置项 | 默认 | 作用 |
|---|---|---|
| `max_message_length` | `0`（不限） | 消息正文字节上限 |
| `max_field_length` | `0`（不限） | 单个字段值字节上限 |
| `max_record_size` | `1024` | 整条记录字节预算 |
| `max_stacktrace_length` | `512` | 单条堆栈字节上限 |

预算超限时按「先丢尾部字段 → 再截断消息 → 最少形态」逐级降级；截断按 UTF-8 字符边界，
不会切出半个字符。

### 聚合去重

同一线程内，相同 `(level, file, line)` 的连续重复日志只输出首条，序列结束时补一条摘要：

```cpp
cfg.dedup_window_ms = 1000;   // 0 = 关闭（默认）
```

```text
... connectionpool.cpp:88 timeout (repeated 1543 times)
```

键取 `(level, file, line)` 而不是消息正文——它在级别过滤之后立刻可算，不依赖格式化；
且正好对应「同一处代码在循环里刷屏」这个真实场景。状态是 `thread_local`，热路径零锁。
JSON 下次数是独立的 `repeated` 成员，便于下游聚合。

### 自身指标

`Logger::stats()` 返回一份 `LogStats` 快照。计数用原子累加，读取不加锁（仅队列长度需短暂持锁）。

| 字段 | 含义 |
|---|---|
| `written` | 成功写入 sink 的记录数（至少一个 sink 成功） |
| `failed_writes` | sink 写失败次数 |
| `dropped` | 丢弃次数（队列满、写失败按策略丢弃、编码抛异常） |
| `by_level[7]` | 按级别统计写入条数，下标同 `LogLevel` |
| `queue_length` | 快照时刻队列长度（同步模式恒为 0） |
| `queue_peak` | 队列峰值 |
| `max_write_latency_us` | 异步写出最长延迟（微秒），从打点到写出 |
| `masked_fields` | 脱敏字段数 |
| `dedup_suppressed` | 被聚合去重抑制的条数 |

```cpp
const LogStats s = Logger::get_instance().stats();
LOG_INFO("written={} dropped={} queue_peak={}", s.written, s.dropped, s.queue_peak);
```

### 运行期改级

```cpp
Logger::get_instance().set_level(LogLevel::DEBUG);   // 接到 HTTP / SIGHUP 里即可
```

线程安全，立即对热路径生效。只对**新记录**生效：已构造 / 已入队的记录持有各自的配置快照。
持久化与鉴权是应用的职责，库只负责线程安全地改。

## 配置

| 字段 | 默认值 | 说明 |
|---|---|---|
| `log_level` | `LogLevel::TRACE` | 全局最低输出级别 |
| `max_message_length` | `0` | 消息正文字节上限（`0` = 不限，超长截断） |
| `max_field_length` | `0` | 单个字段值字节上限（`0` = 不限，超长截断） |
| `max_record_size` | `1024` | 整条记录字节预算（`0` = 不限）。框架与消息的 `...` 标记必定输出，小于「框架 + ...」时以最少形态输出 |
| `stacktrace` | `StackTraceMode::FATAL` | 自动采集堆栈的时机：`OFF` / `FATAL`（仅 FATAL 采）/ `ALWAYS` |
| `stacktrace_depth` | `10` | 自动采集时的最大帧数 |
| `max_stacktrace_length` | `512` | 单条堆栈渲染后的字节上限（`0` = 不限，超出按帧丢弃并附 `(+N frames)`） |
| `dedup_window_ms` | `0` | 聚合去重窗口（毫秒，`0` = 关闭）。同线程内相同 `(level, file, line)` 只输出首条，序列结束补一条重复次数 |
| `enable_sensitive_field_mask` | `false` | 是否启用敏感字段脱敏 |
| `sensitive_field_masker` | `default_sensitive_field_masker` | 脱敏函数 `(key, value) -> value`，入参为未编码的原始值 |
| `time_format` | `TimeFormat::ISO8601` | 时间格式 |
| `use_utc_time` | `false` | 是否使用 UTC（否则本地时间） |
| `format` | `LogFormat::TEXT` | 输出格式：`TEXT` / `JSON` |
| `log_fail_strategy` | `LogFailStrategy::FallbackToStderr` | 写失败策略：`FallbackToStderr` / `Drop` |
| `async` | `false` | 异步写入开关（`set_config` 时惰性启动后台线程） |
| `buffer_size` | `10000` | 异步队列最大长度 |
| `asy_que_ful_strategy` | `AsyQueFulStrategy::Block` | 队列满策略：`Block` / `DropNewest` / `DropOldest` / `DropDebug` |

```cpp
LogConfig cfg;
cfg.log_level = LogLevel::INFO;
cfg.format = LogFormat::JSON;
Logger::get_instance().set_config(cfg);
```

字节大小可用字面量书写（`#include <logger/literals.h>`）：`10_mb` / `512_kb` / `1_gb`。

## 输出格式

文本（默认）：

```
2026-09-04T11:50:32.077[INFO][<thread_id>][<file>:<line>][<func>]<content> key=value ...
```

JSON（`cfg.format = LogFormat::JSON`）：

```json
{"time": "2026-09-04T11:50:32.077", "level": "info", "msg": "order created", "order_id": "ORD-1001", "amount": 99.5, "thread_id": "...", "file": "main.cpp", "line": 42, "func": "main"}
```

成员顺序：`time` / `level` / `msg` → 结构化字段 → 调试信息（`thread_id` / `file` / `line` / `func`）。
字段保类型：数字是 JSON number，字符串会转义 `"` `\` 和控制字符；浮点的 nan / inf 按 JSON 规范写成 `null`。

## 文件输出

```cpp
#include <logger/sink/file_sink.h>

FileSinkConfig fc;
fc.dir = "/var/log/app";                 // 日志目录
fc.date_interval_h = 24;                 // 按时间切分间隔（0 = 只按大小）
fc.max_file_size = 100 * 1024 * 1024;    // 单文件最大字节（0 = 只按时间）
fc.max_backups = 10;                     // 最多保留文件数（0 = 不删除）
Logger::get_instance().add_sink(std::make_shared<FileSink>(fc));
```

- 文件名：日期切分用 `YYYY_MM_DD_HH.log`，大小切分在其后追加 `.1` / `.2` …
- 目录缺失自动创建；文件被外部删除后，下次写入自动重开
- 写失败（磁盘满 / 权限 / 句柄失效）不会崩溃，由 `log_fail_strategy` 兜底，并累计 `Logger::stats()`

轮转与保留策略细节见 [文件轮转指南](docs/guides/file-rotation.md)。

## 同步与异步

默认同步写入（API 返回即写完）。开启异步后，业务线程只把日志压入内存队列即返回，
由后台线程负责格式化与写 Sink，适合高并发高吞吐场景：

```cpp
LogConfig cfg;
cfg.async = true;
cfg.buffer_size = 10000;                         // 队列最大长度
cfg.asy_que_ful_strategy = AsyQueFulStrategy::Block;
Logger::get_instance().set_config(cfg);          // 惰性启动后台线程

LOG_INFO("async message {}", 42);
Logger::get_instance().flush_all();              // 等队列清空并写完
```

队列满时的行为：

| 策略 | 队列满时行为 |
|---|---|
| `Block` | 阻塞调用方，尽量不丢日志 |
| `DropNewest` | 丢弃新日志 |
| `DropOldest` | 丢弃最旧日志 |
| `DropDebug` | 优先丢弃低级别日志，保留新日志 |

> 异步一旦开启即不可回退（后台线程常驻）。需要「不丢」保证时用 `Block`；
> 需要「业务线程绝不被阻塞」时用后三种，代价是计数进 `dropped`。

优雅关闭：`close()`（或析构）停止接收新日志、等待队列消费完、刷新 Sink 并补发未结束的
去重摘要，返回统计信息。`close()` 可重复调用、可并发调用。

选型取舍见 [同步与异步指南](docs/guides/sync-and-async.md)。

## 测试

```bash
cmake -S . -B build          # BUILD_TESTS 默认 ON
cmake --build build
ctest --test-dir build
```

7 个 ctest 目标：

| 目标 | 内容 |
|---|---|
| `test_logger` | 单元 + 集成（级别、过滤、`{}` 格式化、字段、JSON、文本、去重、文件轮转、失败策略、脱敏、注入、长度、指标、上下文、异常、堆栈、Trace、并发） |
| `test_console` | 手动观察 stderr 分流 + 崩溃处理器（子进程真实崩溃） |
| `test_async` | 异步模式（并发无损、四种队列满策略） |
| `test_async_close` | 异步优雅关闭 |
| `test_io_failure` | 权限、目录消失、写失败降级、fd 泄漏 |
| `test_lifecycle` | 并发 close、重复 close、线程回收 |
| `test_soak` | 固定时长持续高并发（默认 2s，`LOGGER_SOAK_SECONDS` 可调大） |

### 性能基准

基于 Google Benchmark（可选依赖，未安装则跳过基准目标），输出吞吐（条/秒）、
P50/P95/P99 延迟与多轮重复的变异系数：

```bash
sudo apt install libbenchmark-dev
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release   # 性能必须在 Release 下测
cmake --build build-release --target logger_benchmark
./build-release/logger_benchmark

./build-release/logger_benchmark --benchmark_filter=Sync          # 只跑一组
./build-release/logger_benchmark --benchmark_repetitions=5 \
    --benchmark_report_aggregates_only                            # 看跨轮稳定性
```

16 个场景：关闭级别、同步文本 / JSON / 字段 / 上下文 / 脱敏 / 去重 / 长度预算 / 转义、
异常记录、堆栈采集与符号化、并发、文件 Sink、异步入队与队列满丢弃。
实测数据、口径说明与结论见 [性能报告](docs/performance.md)。

### Sanitizer

CMake 提供 `LOGGER_SANITIZE` 开关（`thread / address / undefined / leak`）：

```bash
sudo sysctl vm.mmap_rnd_bits=28   # 仅 thread 需要（WSL2 / 新内核）
cmake -S . -B build-tsan -DLOGGER_SANITIZE=thread
cmake --build build-tsan
ctest --test-dir build-tsan
```

> 仅 `thread` 需要 `vm.mmap_rnd_bits=28`，`address / undefined / leak` 不需要。

## 目录结构

```text
logger/
├── CMakeLists.txt
├── Doxyfile                        # API 文档配置（cmake -DBUILD_DOCS=ON --target docs）
├── include/logger/                 # 公共 API（头文件即接口承诺）
│   ├── logger.h                    # Logger 单例 + LOG_* 宏 + with()
│   ├── level.h                     # 级别枚举
│   ├── config.h                    # 配置、LogStats、脱敏规则
│   ├── field.h                     # KV 字段 + encode 编码入口
│   ├── literals.h                  # 容量字面量（10_mb 等）
│   ├── version.h                   # 版本号（LOG_VERSION 等）
│   ├── context.h                   # ContextScope 作用域上下文
│   ├── stacktrace.h                # 调用栈采集
│   ├── trace.h                     # W3C traceparent 编解码
│   ├── crash_handler.h             # 崩溃信号处理
│   ├── sink.h                      # Sink 抽象 + SinkInput
│   ├── sink/console_sink.h         # 控制台 Sink（按级别分流 stdout / stderr）
│   ├── sink/file_sink.h            # 文件 Sink（按日期 / 大小轮转）
│   └── detail/                     # 内部实现，不承诺稳定，不进 API 文档
│       ├── utils.h                 # 时间 / 转义 / UTF-8 截断
│       ├── error.h                 # 异常信息提取
│       ├── dedup.h                 # 聚合去重状态机
│       ├── record.h                # 日志记录
│       ├── format.h                # {} 位置参数格式化
│       └── formatter/              # 文本 / JSON 格式化器
├── src/                            # 实现，目录结构与 include/ 一一对应
│   ├── logger.cpp  field.cpp  stacktrace.cpp  trace.cpp  crash_handler.cpp
│   ├── detail/
│   │   ├── utils.cpp
│   │   └── formatter/
│   └── sink/
├── examples/                       # 可运行示例
├── benchmark/                      # 性能基准（Google Benchmark，可选依赖）
├── test/                           # 单元 + 集成测试
│   ├── test_helpers.h              # 测试用 Sink 等辅助
│   ├── unit/
│   └── integration/
└── docs/                           # 设计文档与指南
```

`include/logger/detail/` 下的内容随时可能改，不承诺接口稳定。公共头只有十来个，
按需 include 即可（不提供 umbrella header）。

## 文档索引

| 文档 | 内容 |
|---|---|
| [结构化日志指南](docs/guides/structured-logging.md) | 字段类型、命名规范、非法字段、转义规则 |
| [上下文指南](docs/guides/context.md) | `ContextScope`、字段优先级、跨线程传递 |
| [错误与堆栈指南](docs/guides/errors-and-stacktrace.md) | 异常展开、嵌套链、`StackTrace` 采集与符号化 |
| [Trace 集成指南](docs/guides/trace.md) | traceparent 编解码、链路接续、与后端对接 |
| [脱敏指南](docs/guides/masking.md) | 内置规则、自定义 masker、命中统计 |
| [同步与异步指南](docs/guides/sync-and-async.md) | 两种模式取舍、队列满策略、关闭语义 |
| [文件轮转指南](docs/guides/file-rotation.md) | 切分策略、保留数量、异常自愈 |
| [性能报告](docs/performance.md) | 实测吞吐与延迟、与目标对照、复现方法 |
| [故障排查](docs/guides/troubleshooting.md) | 看不到日志、丢日志、崩溃、性能异常 |
| [兼容性说明](docs/compatibility.md) | API / ABI 边界、宏前缀、配置与 JSON 字段兼容 |
| [破坏性变更清单](docs/breaking-changes.md) | 历次已发生的破坏性变更 |
| [CHANGELOG](CHANGELOG.md) | 逐版本变更记录 |
| [milestone.md](docs/milestone.md) | 完整设计文档与路线图 |

API 文档用 Doxygen 生成（头文件注释即 API 文档）：

```bash
sudo apt install doxygen
cmake -S . -B build -DBUILD_DOCS=ON
cmake --build build --target docs      # 产物在 build/docs/html/index.html
```

## 提交规范

Commit message 遵循 [Conventional Commits](https://www.conventionalcommits.org/)：

```
<type>(<scope>): <subject>
```

`type` 取值：

| type | 含义 | 示例 |
|---|---|---|
| `feat` | 新功能 | `feat(logger): 添加按级别路由` |
| `fix` | 修复 bug | `fix(formatter): 修复时间格式` |
| `docs` | 仅文档变更 | `docs: 补充 README` |
| `style` | 代码风格（空格/格式/分号，不影响逻辑） | `style: 统一缩进` |
| `refactor` | 重构（不改行为、不加功能、不修 bug） | `refactor: 拆分 log()` |
| `perf` | 性能优化 | `perf: 减少字符串拷贝` |
| `test` | 添加/修改测试 | `test: 添加并发测试` |
| `build` | 构建系统或外部依赖 | `build: 升级 CMake` |
| `ci` | CI 配置 | `ci: 添加 GitHub Actions` |
| `chore` | 杂务（不涉及 src/test 的维护） | `chore: 更新 .gitignore` |
| `revert` | 回滚某次提交 | `revert: 回滚 feat(logger)` |

- `scope`：可选，影响范围（如 `logger`、`formatter`、`sink`、`test`）
- `subject`：简短描述，祈使语气，≤ 50 字

本地用 pre-commit 的 `commit-msg` 钩子校验：

```bash
pre-commit install --hook-type commit-msg
```

## 里程碑

完整路线见 [docs/milestone.md](docs/milestone.md)。

| 版本 | 里程碑 | 状态 |
|---|---|---|
| v0.1.0 | M1 基础日志 | ✅ |
| v0.2.0 | M2 结构化日志 | ✅ |
| v0.3.0 | M3 生产输出 | ✅ |
| v0.4.0 | M4 异步、并发与高性能 | ✅ |
| v0.5.0 | M5 上下文、错误与链路追踪 | ✅ |
| v0.6.0 | M6 安全、可靠性与生产能力 | ✅ |
| v1.0.0 | M7 测试、文档与正式发布 | ✅ |
