
---

# 一、项目目标

最终建设一个具备以下能力的 C++ Logger：

- 支持 Debug、Info、Warn、Error 等日志级别
- 支持结构化日志
- 支持文本和 JSON 格式
- 支持多种输出目标
- 支持同步和异步写入
- 支持上下文、请求 ID、Trace ID
- 支持文件轮转
- 支持错误堆栈
- 支持日志聚合去重与丢弃策略
- 支持敏感数据脱敏
- 支持动态修改日志级别
- 具备完整测试、性能指标和文档
- 在高并发场景下稳定运行
- 日志故障不能影响主业务
- 日志代码路径不向业务抛异常（noexcept 边界）

---

# 二、总体规划

可以按照以下阶段推进：

| 阶段 | 目标 | 建议周期 |
|---|---|---:|
| M0 | 项目立项与技术设计 | 3～5 天 |
| M1 | 最小可用版本 MVP | 1～2 周 |
| M2 | 结构化日志与格式化系统 | 1～2 周 |
| M3 | 输出系统与文件管理 | 2 周 |
| M4 | 异步、并发与高性能 | 2～3 周 |
| M5 | 上下文、错误与可观测性 | 1～2 周 |
| M6 | 安全、可靠性与生产能力 | 2 周 |
| M7 | 完整测试、文档与发布 | 1～2 周 |
| M8 | 生态扩展与高级能力 | 持续迭代 |

整体第一版生产可用版本可以控制在 **10～16 周**。

---

# 三、M0：项目立项与技术设计

## 目标

在正式编码前明确：

- 项目边界
- API 设计
- 数据模型
- 输出模型
- 性能目标
- 可靠性策略
- 版本规划

## 主要任务

### 1. 明确使用场景（决策记录）

| 问题 | 决策 | 理由 |
|---|---|---|
| 部署形态 | 单体应用为主，兼顾微服务 | 单例 Logger 贴合单体；微服务只是每进程一份日志 + trace 字段，不影响核心设计 |
| 输出目标 | stdout 默认 + 文件可选 | stdout 贴合容器/云原生，文件是传统部署刚需；Sink 抽象两者都覆盖 |
| 远程日志平台 | 第一版不做，仅留 Sink 扩展点（M8） | 避免范围膨胀 |
| 异步日志 | MVP 同步，M4 增加异步（可选开关） | 先做对再做快；异步复杂度放到独立里程碑 |
| 多进程 | 不主动保证 fork 安全，约定单进程单文件 | fork 后 fd 共享、轮转竞争复杂；多进程部署用 pid/主机名区分文件 |
| 兼容现有 API | 不兼容，统一自研风格 | 兼容 glog/spdlog 会束缚宏命名与语义 |
| 链路追踪 | 预留 trace_id/span_id 字段，不强依赖 OTEL | 字段层预留成本低；强集成放 M8 |
| 动态改级 | 支持 | set_level 已有，加 HTTP/SIGHUP 触发即可，排障刚需 |
| 是否允许丢日志 | 同步不丢；异步可丢（Error/Fatal 走同步降级） | 给业务确定性预期 |
| C++ 标准 | C++17（现状） | 自己实现 format，不依赖 C++20 的 std::format |
| 库形态 | 静态库 + header-only 模板 | 模板放头文件，非模板实现编进静态库（当前 CMake 形态） |
| 日志接口 | 宏接口（不用 std::source_location） | C++17 无 source_location；宏用 LOG_ 前缀防冲突 |

### 2. 确定日志级别

与当前 `LogLevel` 枚举一致：

| 级别 | 适用场景 |
|---|---|
| Trace | 极细粒度的执行过程 |
| Debug | 开发调试信息 |
| Info | 正常业务流程 |
| Warn | 可恢复异常或潜在风险 |
| Error | 当前操作失败 |
| Fatal | 致命错误，记录后通常调用 `std::abort()` 终止进程 |
| Off | 关闭所有日志输出 |

### 3. 设计核心 API

同时提供宏接口和结构化接口。宏接口（当前已实现，snprintf 风格）：

```cpp
LOG_INFO("user %d login", userID);
```

格式化接口（自己实现format,不用库函数）：

```cpp
LOG_INFO("user {} login", userID);
```

结构化接口：

```cpp
Logger::get_instance().info("user login",
    KV("user_id", userID),
    KV("ip", ip),
);
```

上下文接口（thread_local + RAII 作用域对象为主，`with()` 子 Logger 为辅）：

```cpp
ContextScope ctx{ {"request_id", requestID}, {"trace_id", traceID} };  // RAII：入作用域生效，出作用域自动清除
LOG_INFO("request completed, status={}", 200);
// 出作用域，ctx 析构自动出栈
```

上下文字段在 `log()` 内、入异步队列前就合并进 Record；thread_local 不跨线程，跨线程需用 `with()` 子 Logger 传值。

子 Logger（预绑定字段）：

```cpp
auto requestLogger = Logger::get_instance().with(
    KV("request_id", requestID),
    KV("trace_id", traceID),
);
```

生命周期接口：

```cpp
Logger::get_instance().flush();   // 显式刷盘
// 析构函数自动 flush + close（RAII）
```

### 4. 设计核心抽象

建议拆成以下模块：

```text
Logger
 ├── Level Filter
 ├── Context Fields
 ├── Record
 ├── Formatter
 │    ├── Text Formatter
 │    └── JSON Formatter
 └── Sink
      ├── Stdout Sink
      ├── Stderr Sink
      ├── File Sink
      └── Multi Sink
```

可以定义如下数据流：

```text
调用日志 API
      ↓
判断日志级别（编译期可过滤）
      ↓
构造 Record
      ↓
附加上下文字段
      ↓
字段脱敏
      ↓
格式化（Formatter）
      ↓
写入 Sink
```

### 5. 技术选型建议

| 方面 | 建议 |
|---|---|
| C++ 标准 | C++17（现状） |
| 构建系统 | CMake + CTest |
| 格式化 | 自研轻量格式化（`{}` 占位符），不引入 {fmt} / `std::format` 库函数 |
| JSON 编码 | 自研轻量编码器（避免依赖、性能可控）|
| 堆栈采集 | backward-cpp（header-only，自带符号化）；零依赖场景用 glibc `backtrace()` 起步；C++23 再考虑 `std::stacktrace` |
| 单元测试 | GoogleTest（已在用） |
| 性能测试 | Google Benchmark |
| 包管理 | CMake + FetchContent，或 vcpkg / Conan |
| 参考实现 | spdlog、glog、log4cplus 的设计取舍 |

## 交付物

- 项目 README 初稿
- 架构设计文档
- API 设计文档（头文件草案）
- 日志字段规范
- 错误处理策略
- 性能目标
- 版本规划
- 风险清单

## 验收标准

- 核心 API 已确定（宏、格式化、结构化接口边界清晰）
- 同步、异步模式的边界明确
- 日志是否允许丢失有明确答案
- 输出失败处理策略已确定
- header-only 与编译进库的部分划分明确
- 第一版和后续版本范围明确

---

# 四、M1：最小可用版本 MVP

## 目标

实现一个可以被业务项目直接使用的基础 Logger（当前代码已覆盖大部分）。

## 主要功能

### 1. 基本日志级别

实现：

```cpp
LOG_DEBUG("debug message");
LOG_INFO("info message");
LOG_WARN("warn message");
LOG_ERROR("error message");
```

### 2. 全局日志级别

例如：

```cpp
Logger::get_instance().set_level(LogLevel::INFO);
```

当级别为 `INFO` 时：

- Debug 不输出
- Info 输出
- Warn 输出
- Error 输出

级别判断应在宏入口处尽早完成，避免构造 `Record` 等昂贵操作。

### 3. 标准输出

第一版只需要支持：

- stdout（ConsoleSink）
- stderr（Error 及以上级别可单独路由）

例如：

```cpp
Logger::get_instance().add_sink(std::make_shared<ConsoleSink>());
```

### 4. 基本文本格式

输出示例：

```text
2026-09-02T10:20:30.123Z [INFO] user login user_id=1001
```

至少包含：

- 时间
- 日志级别
- 线程 ID（可选）
- 文件、行号、函数（可选，调试期）
- 消息
- 字段

### 5. 并发安全

保证多个线程同时写日志时：

- 不发生数据竞争（用 ThreadSanitizer 验证）
- 不发生内容交叉
- 不发生崩溃
- 日志记录不会互相覆盖

### 6. 基础配置

支持代码配置：

```cpp
LoggerConfig config;
config.level = LogLevel::INFO;
config.time_format = TimeFormat::ISO8601;  // 时间格式
Logger::get_instance().configure(config);
```

## 验收标准

- 可以正常输出四种日志级别
- 日志级别过滤正确
- 多线程并发调用无数据竞争（TSan 通过）
- 输出格式稳定
- 基本 API 有单元测试（GoogleTest）
- README 中有可运行示例
- 能够通过 CMake 作为静态库被其他项目链接

## 建议版本

```text
v0.1.0
```

---

# 五、M2：结构化日志与格式化系统

## 目标

将 Logger 从“打印字符串”升级为“记录结构化事件”。

## 主要功能

### 1. 自研 {} 格式化器

在进入 KV 之前，先把现有 `snprintf`（`%d`）风格的格式化替换为自研 `{}` 占位符格式化器，作为 KV 与 JSON 输出的公共地基：

- `encode(value, out, bool json)`：底层编码入口，文本与 JSON 共用，仅在字符串处按 `json` 分支（加引号/转义）
- `format(fmt, args...)`：位置参数内插，复用 `encode`
- 不引入 {fmt} / `std::format`，自研实现
- TextFormatter 与 JsonFormatter 必须复用同一 `encode` 路径（文本 `key=value`，JSON 输出带类型成员）
- 替换现有 `log()` 的 `snprintf` 路径后，同步更新 README、examples 与 M1 测试用例再回归

> 破坏性变更说明：本里程碑把格式化语义从 printf（`%d`/`%s`）切换为 `{}`，v0.1.0 的日志格式字符串不再源级兼容。`LOG_INFO("user %d login", id)` 在新语义下会变成「正文原样 + 参数无处安放」，必须同步改写为 `LOG_INFO("user {} login", id)`。

```cpp
LOG_INFO("user {} login", userID);
```

### 2. Key-Value 字段

支持：

```cpp
Logger::get_instance().info("order created",
    KV("order_id", orderID),
    KV("user_id", userID),
    KV("amount", amount),
);
```

### 3. JSON Formatter

输出示例：

```json
{
  "time": "2026-09-02T10:20:30.123",
  "level": "info",
  "msg": "order created",
  "order_id": "ORD-1001",
  "user_id": 2001,
  "amount": 99.5
}
```

> 时间序列化统一走 `format_time`，输出带毫秒的规范时间串（如 `2026-09-02T10:20:30.123`），文本与 JSON 共用；不要在 TextFormatter / JsonFormatter 里各自拼接时间。

### 4. 预绑定字段

支持：

```cpp
auto serviceLogger = Logger::get_instance().with(
    KV("service", "order-service"),
    KV("version", "1.2.0"),
);

serviceLogger.info("server started");
```

输出：

```text
service=order-service version=1.2.0
```

### 5. 自定义字段类型

支持：

- 字符串（`const char*`、`std::string`、`std::string_view`）
- 整数（各宽度有符号/无符号）
- 浮点数（`float` / `double`，正确处理 `nan` / `inf`）
- 布尔值
- 时间（`std::chrono` 各时钟的时间点）
- 数组与容器
- 自定义序列化类型（`operator<<`）

> 对象嵌套结构、异常（`std::exception` / `std::error_code`）不在本里程碑，归入 **M5：错误记录**（见第八节），避免 M2 范围膨胀、与 M5 重叠。

### 6. 字段命名规范

建议统一：

```text
time
level
msg
service
version
host
pid
thread_id
request_id
trace_id
span_id
error
stacktrace
```

### 7. 非法字段处理

C++ 有强类型系统，大部分字段错误可以在编译期拦截：

```cpp
logger.info("test", KV("", value));       // static_assert：空 key 编译失败
logger.info("test", KV("key", my_type));  // 无序列化支持 → 编译错误
```

需要明确处理以下情况：

- 空 key：字面量 key 编译期 `static_assert` 拦截；运行时动态 key 在 log 阶段直接跳过
- 重复 key：后写覆盖（last-write-wins），保序去重
- 重载决议歧义（如 `const char*` 指针 vs 字符串）
- 运行时兜底（如 `std::bad_alloc` 等异常）

建议通过模板 + `static_assert` / `if constexpr` 在编译期拒绝非法字段，运行时兜底用 noexcept 包裹，绝不让日志 API 因字段错误直接导致业务崩溃。

### 8. 特殊字符转义

处理：

- 换行符
- 制表符
- 引号
- 控制字符
- 用户输入中的日志注入内容

## 验收标准

- 同一 Record 在文本与 JSON 下字段值语义一致（值不变，仅序列化形式不同），不要求字节级一致
- 字段类型不会被无意义地全部转成字符串（int 输出 `1001` 而非 `"1001"`）
- 字段顺序有明确规则：按调用顺序排列，用 `std::vector<Field>` 等有序容器实现，不得用无序 map
- TextFormatter 与 JsonFormatter 复用同一 `encode(value, out, bool json)` 编码路径
- 非法字段在编译期被拦截，运行时兜底不崩溃
- 特殊字符不会伪造日志行
- 结构化字段功能有完整测试（各类型 encode、`{}` 插值、JSON 转义、字段顺序、类型保留）

## 建议版本

```text
v0.2.0
```

---

# 六、M3：输出系统与文件管理

## 目标

支持生产环境常见的日志输出方式和日志文件管理。

## 主要功能

### 1. 多 Sink 输出

支持：

```text
Logger → stdout
Logger → file
Logger → stdout + file
Logger → custom sink
```

例如：

```cpp
Logger::get_instance().add_sink(std::make_shared<ConsoleSink>());
Logger::get_instance().add_sink(
    std::make_shared<FileSink>(FileSinkConfig{"/var/log/"}));
```

### 2. 自定义 Sink

允许用户继承抽象接口接入自己的输出目标：

```cpp
class LogSink {
public:
    virtual ~LogSink() = default;
    virtual bool log(const SinkInput& input) = 0;  // 返回是否写入成功
    virtual void flush() = 0;
};
```

### 3. 文件轮转

支持按大小切分：

```text
app.log
app.log.1
app.log.2
```

支持按时间切分：

```text
app-2026-09-01.log
app-2026-09-02.log
```

支持大小与时间结合的轮转

### 4. 日志保留策略

支持配置：

- 单文件最大大小
- 最大保留文件数

例如（配合当前 `logger::literals` 字面量）：

```text
max_size=100_mb
max_backups=10
```

### 5. 文件异常处理

考虑：

- 目录不存在
- 权限不足
- 磁盘空间不足
- 文件被外部删除
- 文件被重命名
- 文件句柄失效（ofstream 状态检测）
- 轮转失败

### 6. 日志输出失败策略

提供配置：

```text
FallbackToStderr //降级到stderr
Drop             //丢弃该日志
```

生产环境建议默认：

- 普通日志写入失败时避免阻塞业务
- Error 日志尽量降级到 stderr
- 统计输出失败次数
- 不允许日志系统导致进程崩溃

## 验收标准

- 支持 stdout、stderr、文件和自定义 Sink
- 文件轮转功能稳定
- 不会因为文件不存在导致无法继续写入
- 输出失败时符合预期降级策略
- 文件权限和清理策略有测试
- 多 Sink 下同一条日志不会重复或错乱

## 建议版本

```text
v0.3.0
```

---

# 七、M4：异步写入、并发与性能优化

> **状态：已完成（v0.4.0）**。异步写入（`async` 开关 + 后台线程 + 五种队列满策略）、
> 优雅关闭（`close()` 刷出剩余日志）均已实现。测试覆盖见 `test_async` / `test_async_close`，
> 性能基准见 `benchmark/logger_benchmark.cpp`（Google Benchmark，可选依赖；
> 输出吞吐 + P50/P95/P99，实测数据见 [performance.md](performance.md)）。
> 本机（WSL2, Debug 构建）参考结果：关闭级别 7.5M 条/秒、同步文本 454K 条/秒、
> 异步文本 466K 条/秒、同步 JSON 222K 条/秒、文件 Sink 488K 条/秒，关键路径 P99 < 10µs。

## 目标

让 Logger 能够应对高并发和高吞吐场景。

## 主要功能

### 1. 同步模式

同步模式特点：

- 日志写入完成后 API 返回
- 日志可靠性较高
- 业务线程会承担写入开销
- 调试和关键日志场景适合使用

### 2. 异步模式

异步模式：

```text
业务线程
   ↓
内存队列
   ↓
后台写入线程
   ↓
Formatter / Sink
```

配置示例：

```cpp
LoggerConfig config;
config.async = true;
config.buffer_size = 10000;
Logger::get_instance().configure(config);
```

### 3. 队列满处理

需要明确策略：

| 策略 | 说明 |
|---|---|
| Block | 阻塞调用方，尽量不丢日志 |
| DropNewest | 丢弃新日志 |
| DropOldest | 丢弃旧日志 |
| DropDebug | 优先丢弃低级别日志 |

推荐支持按级别配置：

```text
Debug：允许丢弃
Info：队列满时丢弃或阻塞
Warn：尽量保留
Error：同步降级
```

### 4. 优雅关闭

实现：

```cpp
Logger::get_instance().flush_all();
Logger::get_instance().close();
```

或者依赖析构函数自动完成（RAII）。关闭时需要：

1. 停止接收新日志
2. 等待队列消费
3. 刷新 Formatter
4. 刷新 Sink
5. 关闭文件或网络连接
6. 返回关闭错误

### 5. 性能优化

重点优化：

- 日志级别关闭时的开销（宏入口处先比较，避免构造 `Record` 和获取时间戳）
- 字符串格式化（自研轻量格式化，减少动态分配与多次拷贝）
- JSON 编码（轻量手写编码器，减少动态分配）
- 临时对象分配（`string_view`、SSO、缓冲区复用）
- 锁竞争（无锁队列、per-thread 缓冲）
- 内存队列
- 批量写入
- 时间获取（缓存秒级时间戳）
- 调用栈获取（惰性采集）

### 6. 性能基准

建议建立以下 Google Benchmark：

```text
BM_InfoDisabled
BM_InfoText
BM_InfoJSON
BM_Concurrent
BM_Async
BM_FileSink
BM_WithFields
BM_ErrorWithStacktrace
```

需要记录：

- 每秒日志条数
- 单条日志平均耗时
- P50/P95/P99 延迟
- 内存占用
- 每条日志分配次数
- CPU 使用率
- 队列峰值
- 丢弃日志数量

## 建议性能目标

具体指标需要根据编译器和硬件调整，可以先设定一个相对目标（可参考 spdlog 的 benchmark）：

| 指标 | 目标 |
|---|---:|
| 关闭级别日志开销 | 尽量低于普通函数调用 |
| 同步文本日志 | ≥ 100K 条/秒 |
| 异步文本日志 | ≥ 300K 条/秒 |
| 关键路径额外延迟 | P99 小于 1ms |
| 无异常情况下 | 不发生日志丢失 |
| 关闭时 | 队列可完整刷出 |

这些指标不应盲目追求，最终应以真实业务场景为准。

## 验收标准

- 同步、异步模式均可用
- 队列满时行为符合配置
- Close 能够正确刷出剩余日志
- 无死锁、数据竞争和线程泄漏（TSan 通过）
- 有稳定的 benchmark
- 性能相较基础版本没有明显回退
- 关闭日志级别时不会执行昂贵计算

## 建议版本

```text
v0.4.0
```

---

# 八、M5：上下文、错误与链路追踪

## 目标

让日志能够关联请求、用户和分布式调用链。

## 主要功能

### 1. 上下文支持

C++ 没有语言级 context，推荐以 **thread_local + RAII 作用域对象**为主，`with()` 子 Logger 为辅：

```cpp
// log_context.h
struct Field { std::string key; std::string value; };  // 第一版 string，M2 扩展强类型 KV

class ContextScope {                                    // RAII：构造入栈，析构出栈
public:
    explicit ContextScope(std::initializer_list<Field> fields);
    ~ContextScope();
    ContextScope(const ContextScope&) = delete;         // 不可拷贝，避免出栈顺序错乱
    ContextScope& operator=(const ContextScope&) = delete;
};

// Logger 维护 thread_local 上下文栈，log() 在构造 Record 时合并当前所有帧字段
thread_local static inline std::vector<std::vector<Field>> context_;
```

业务用法：

```cpp
void handle_request(const Request& req) {
    ContextScope ctx{ {"request_id", req.id}, {"trace_id", req.trace} };
    LOG_INFO("handling request");                       // 自动带 request_id / trace_id
}
```

实现要点：

- 异步「调用点物化」：thread_local 字段必须在业务线程的 `log()` 内、入队列前合并进 Record，后台写入线程读不到业务线程的 thread_local
- thread_local 不跨线程：子线程 / 线程池拿不到父线程上下文；需延续时显式带字段，或用 `with()` 子 Logger 传值
- 先过滤后合并：级别判断放在合并上下文之前，关闭级别时不产生 thread_local 读和字段拷贝开销
- 字段优先级（同 key 覆盖规则，**固定不变**）：

  ```
  显式 KV  >  ContextScope  >  with() 预绑定
  ```

  合并顺序即 `with() 字段 → 上下文各帧 → 调用点显式 KV`，`dedup_fields` 的「后写覆盖、位置取首次出现」自然实现该优先级。
  即：`with()` 是对象级默认值，`ContextScope` 是环境状态，可覆盖前者；调用点显式传来的最优先。
  上下文各帧之间是**栈序**：后入栈的帧覆盖先入栈的（内层覆盖外层）。
  不采用「按书写先后取号排序」的时序优先——那会让输出依赖 `with()`/`ContextScope` 在源码中的位置，重构时静默改变日志内容，且新使用者无从得知规则。

自动提取：

- request ID
- trace ID
- span ID
- user ID
- tenant ID

### 2. 请求级 Logger

提供辅助方法预绑定字段：

```cpp
auto requestLogger = Logger::get_instance().with(
    KV("request_id", requestID),
    KV("method", method),
    KV("path", path),
);
```

### 3. 错误记录

一条错误日志有三种写法，只有第三种触发「自动展开」：

```cpp
LOG_ERROR("db query failed: {}", e.what());       // ① 纯字符串：logger 只记录 msg，不做自动处理
LOG_ERROR("db query failed", KV("err", e));        // ② 单字段：err=<what()>，一个 KV 一个字段，不展开
LOG_EXCEPTION("db query failed", e);               // ③ 自动展开：一个异常 → 固定字段集
```

约定：`KV("err", e)` 永远只产生一个字段（值为 `what()`），不破坏「一个 KV = 一个字段」的模型；
「自动记录」走独立的 `LOG_EXCEPTION` 入口。传字符串（写法 ①）不会触发自动记录——C++ 无法从字符串回溯异常对象。

`LOG_EXCEPTION` 把异常自动展开成如下字段：

| 字段 | 内容 | 何时出现 |
|---|---|---|
| `msg` | 传入的说明文字 | 总是 |
| `error` | 最内层异常的 `what()` | 总是 |
| `error_type` | 可读类型名（demangle 后），如 `std::system_error` | 总是 |
| `error_chain` | 完整嵌套链：外层 `what()` 换行 `  caused by: ` 内层 `what()` | 有嵌套时 |
| `stacktrace` | 采集点堆栈 | 仅 Fatal / 显式请求 |

JSON 输出示例：

```json
{
  "level": "error",
  "msg": "database query failed",
  "error": "connection refused",
  "error_type": "std::system_error",
  "error_chain": "std::runtime_error: db query failed\n  caused by: std::system_error: connection refused"
}
```

实现规则：

- **error type**：`std::exception` 用 `<cxxabi.h>` 的 `abi::__cxa_demangle` 解出可读类型名（Linux 下 GCC/clang 通用，无新依赖）；`std::error_code` 输出 `message (value) [category]`。两套来源各走各的编码，不混。
- **嵌套链 = wrapped error**：只实现标准 `std::nested_exception`（`std::rethrow_if_nested` 递归解包，生成 `error_chain`）。「wrapped error」不单独作为一种机制——它要么就是嵌套链，要么已涵盖在 `std::error_code` 的底层信息里。
- **stacktrace**：仅 Fatal 或显式请求时自动采集，普通 Error 默认不采（与「4. 堆栈信息」口径一致）。

### 4. 堆栈信息

实现选型（首选 backward-cpp）：

- backward-cpp：header-only，自带符号化（函数名 demangle、文件、行号），跨平台；Linux 下链接 `-ldl`、编译加 `-rdynamic`
- glibc `backtrace()` / `backtrace_symbols_fd()`：零依赖起步，但只有符号名、无行号，适合先理解原理
- C++23 `std::stacktrace`：升级标准后再考虑，C++17 暂不可用

关键设计——采集与符号化分离：

- 采集（拿返回地址列表）较快；符号化（地址 → 函数名/行号）很慢且非 async-signal-safe
- 普通 Error 默认不采集完整堆栈，或惰性符号化
- Fatal / 崩溃（SIGSEGV / SIGABRT）在 signal handler 里只用 `backtrace()`（async-signal-safe）拿原始地址，符号化放到事后（fork 子进程或 atexit）做，不在信号处理函数里 malloc / 走非 signal-safe 路径

还需明确以下策略：

- Error 是否默认记录堆栈
- 同一错误是否重复记录堆栈
- 堆栈采集开销
- 生产环境是否可关闭
- 崩溃（SIGSEGV / SIGABRT）是否自动记录堆栈

建议：

- 普通 Error 默认不一定采集完整堆栈（采集开销较大）
- Fatal 自动采集
- 崩溃场景通过 signal handler 捕获并记录堆栈
- 提供显式接口记录堆栈

栈顶库帧的处理：

- 只用 `skip`，统一默认跳 1 帧（丢掉 `StackTrace::capture` 自身），不做事后过滤
- 取 1 而非 3：`capture` 跨 TU 不会被内联，恒为第 0 帧，`skip=1` 必定只丢它；
  而「`log_impl`/`log` 各占一帧」是假设——`-O2` 下这两帧被内联掉，多出的 skip
  会开始吃真实用户帧（实测 `main` 会从堆栈里消失）。调试构建多一两帧噪声好过丢用户帧
- 曾尝试按符号名过滤（`Logger::` / `StackTrace::` 前缀），已放弃：无法覆盖库内自由函数，
  且会误伤 `MyLogger::log` 这类同名子串的用户符号——误判的代价（删掉真正出错的那帧）比噪声大
- `skip` 同样**不保证**丢掉的一定是 `StackTrace::capture`：ASan / TSan 会插入自己的
  `backtrace` 拦截帧，`capture` 落到第 1 帧。这只是栈顶多一帧噪声，不影响功能；
  但测试里不要断言"skip=1 后必定不含某帧"，只断言帧数差（工具链无关）

堆栈的预算控制（必须，否则会被整条日志的截断毁掉）：

- 堆栈有**独立预算** `max_stacktrace_length`（默认 512 字节），与 `max_record_size` 分开
- 超预算时**按帧丢弃**（每帧完整），末尾附 `... (+N frames)`
- 原因：堆栈排在字段末尾，若靠 `max_record_size` 兜底做字节级硬切，会切掉行尾 `\n`（TEXT 行结构破坏）、切出非法 JSON（整条被日志管道丢弃），而且用户已经付过采集与符号化开销却看不到内容
- `stacktrace_depth` 默认 10 帧，与预算配合；显式 `StackTrace::capture()` 也带同样的默认预算，避免调用方绕过

崩溃捕获（L3，独立于前两层）：

- 分三层：**L1 采集**（`backtrace()`）/ **L2 符号化**（`dladdr` + demangle）/ **L3 崩溃捕获**（signal handler）
- L3 补的缺口：`LOG_FATAL` 是代码**主动**调用的；进程真崩溃（SIGSEGV / SIGABRT / SIGBUS / SIGFPE / SIGILL）时一条日志都不会有，只剩 `Segmentation fault`
- L3 **不能复用 `StackTrace` 与 `Logger`**：handler 里只能调 async-signal-safe 接口，而
  `StackTrace::str()` 全程分配（std::string / demangle / snprintf），`Logger` 的 mutex 可能正被崩溃线程持有
- 因此 L3 是独立实现：只用 `write()` + 自实现的十六进制/十进制格式化 + `backtrace()`
- 记录内容（**精确现场**）：pid / tid、信号名与编号、`si_code`、故障地址 `si_addr`、
  出错指令指针 `fault_pc`（取自 `ucontext` 的 `REG_RIP`，**不是** `backtrace` 的栈顶——后者是 handler 与信号跳板）、
  `rsp` / `rbp`、栈回溯原始地址
- 每帧附「模块路径+偏移」：偏移是**该模块的链接期地址**，可直接喂 `addr2line`
  - `dl_iterate_phdr` 的 `dlpi_addr` 就是该模块的 load bias（PIE 下为装载基址，非 PIE 下为 0），
    所以 `运行时地址 - dlpi_addr` 对主程序与共享库一律成立，**无需判断是否 PIE**
  - 反面教训：`dladdr` 的 `dli_fbase` 是**第一个 PT_LOAD 段的运行时地址**，不是 load bias；
    非 PIE 下它等于 `0x400000`（段起始 vaddr），减它会少算 0x400000，使 addr2line 静默解析到错误符号
- 其它必要处理：
  - `sigaltstack` 独立信号栈：栈溢出导致的崩溃也能记录
  - 安装时先调一次 `backtrace()` 热身：它属于 libgcc，首次调用触发动态装载（可能 malloc），不能在 handler 内发生
  - 写完后 `signal(SIG_DFL)` + 解除阻塞 + `raise`：**保留 core dump 语义**，事后仍可 gdb
  - 递归崩溃防护（`volatile sig_atomic_t`），失败兜底用 `_exit` 而非 `exit`
- API：`install_crash_handler(path)`（path 为空写 stderr）/ `uninstall_crash_handler()`
- 不做符号化，离线还原：
  ```
  addr2line -f -C -e <模块路径> <偏移>
  ```
- 与已有 handler 的关系（**只接管原本就是默认动作的信号**）：
  - 非默认 handler（ASan/TSan、JVM 运行时）→ 该信号**不接管**，stderr 提示一句
  - `SIG_IGN` → 同样不接管：程序有意忽略该信号，接管会把"忽略"变成"崩溃退出"
  - 为什么不做链式调用：JVM 用 SIGSEGV 实现隐式空指针检查，正常的 null 解引用靠它的
    handler 修正 PC 后恢复执行；链式调用会让我们在**每次 null 检查**时都写一份假崩溃日志
  - 因此接管时该信号一定是 `SIG_DFL`，卸载一律恢复 `SIG_DFL`，无需保存旧 handler
    （只需记住"哪些信号真被接管过"，避免卸载时误动别人的 handler）
  - `install_crash_handler()` 返回"是否至少接管了一个信号"，全部跳过时为 false

### 5. Trace 系统集成

**用户要解决什么**

一个请求跨几个服务、几十条日志，出错后要捞出「这一次请求」的全部记录，靠 grep 关键字是捞不全的。用户的需求分两类：

| 用户处境 | 需求 |
|---|---|
| 已经上了链路追踪（Jaeger / Tempo / SkyWalking / OTEL） | 日志里要带上现有的 `trace_id` / `span_id`——能从日志跳到 trace 看全貌，也能从 trace 跳到日志看细节 |
| 还没上链路追踪 | 至少能把一次请求的多条日志串起来（有个关联 id 就够） |
| 两者的共同底线 | **不要 logger 自带一套追踪实现，不要绑死具体后端** |

第二类用户的现实是：他们可能下个月就上追踪系统了。所以本库不能要求他们先选好厂商。

**怎么满足**

只做「三个固定字段 + W3C traceparent 编解码」，**不引入 OpenTelemetry C++ SDK**（它会连带 abseil / protobuf / gRPC 一整套重量级依赖，而日志侧要的只是字段值）。字段挂载直接复用 M5.1 的上下文机制，不新增传递方式。

字段规范（名字固定，JSON 下均为字符串）：

| 字段 | 格式 | 说明 |
|---|---|---|
| `trace_id` | 32 位小写 hex | 全链路唯一；W3C 规定不得为全 0 |
| `span_id` | 16 位小写 hex | 当前 span；不得为全 0 |
| `trace_flags` | 2 位小写 hex | bit0 为 sampled；`"01"` 表示采样 |

**用户怎么用**

场景一：已有追踪系统，从自己的 span 取值

```cpp
ContextScope ctx{ KV("trace_id", span.trace_id()),
                  KV("span_id", span.span_id()),
                  KV("trace_flags", span.sampled() ? "01" : "00") };
LOG_INFO("handling request");   // 自动带上三个字段
```

场景二：没有追踪系统，起一条新链路

```cpp
TraceContext tc;
if (!parse_traceparent(req.header("traceparent"), tc))
  tc = generate_trace();        // 无上游 → 自己起一条
ContextScope ctx{ KV("trace_id", tc.trace_id), KV("span_id", tc.span_id) };
```

场景三：要往下游服务传

```cpp
http_client.set_header("traceparent", make_traceparent(tc));
```

API：

```cpp
// include/logger/trace.h
// traceparent 的四段都可携带；version / trace_flags 有默认值，构造后不设也能用
struct TraceContext {
  std::string version = "00";      // 2 hex，当前规范仅有 "00"
  std::string trace_id;            // 32 hex，必须由调用方填
  std::string span_id;             // 16 hex，必须由调用方填
  std::string trace_flags = "01";  // 2 hex，bit0 为 sampled
  [[nodiscard]] bool sampled() const;
};

TraceContext generate_trace();                                    // 起新链路
bool parse_traceparent(std::string_view header, TraceContext& out);  // 格式非法返回 false
std::string make_traceparent(const TraceContext& ctx);            // 供下游传播
```

> `version` 参与 traceparent 编解码但不作为日志字段输出——日志只带 `trace_id` / `span_id` / `trace_flags` 三个字段。

输出示例：

```json
{
  "level": "error",
  "msg": "payment failed",
  "trace_id": "4bf92f3577b34da6a3ce929d0e0e4736",
  "span_id": "00f067aa0ba902b7",
  "trace_flags": "01",
  "error": "timeout"
}
```

**明确不做**

- 不实现 span 层级、不采集 span、不上报——那是追踪系统的事
- 不自动注入 HTTP 头，只提供 `make_traceparent` 给调用方用
- 不按 `trace_flags` 丢日志：采样就该是采样本的职责（M6），两者不耦合
- 不反查 `trace_id`，logger 只做字段透传

**解析与生成规则**

- `parse_traceparent` 只接受 4 段及以上；version 为 `00` 时**不得**有追加字段，更高版本的追加字段按规范忽略。
- `make_traceparent` **四个字段一视同仁**：任一长度/字符/全 0 不合法即返回空串，**不做任何默认值替换**。默认值只放在 `TraceContext` 的成员初始值里，不放在生成函数里——否则会发出「看似合法但语义被改过」的头（如把非法的 `trace_flags` 悄悄写成 `00`）。
- `sampled()` 取 `trace_flags` 的 **bit0**，不是「nibble 非 0」。flags 是 8 位标志位，`"02"`/`"0c"` 这类值 bit0 为 0，属未采样。

**异常情况的行为**

- `trace_id` / `span_id` 长度或字符不合法 → 按「无 trace」处理，字段不输出，不报错、不影响其它字段
- `make_traceparent` 生成不合规内容 → 同上，视为无 trace
- 跨线程 / 线程池不延续：`ContextScope` 是 thread_local（与 M5.1 一致），需延续时显式 `with(KV("trace_id", ...))` 传值

**OTEL 集成（M8）**

届时只增加一个 `from_otel_span(span) -> TraceContext` 适配器，核心代码不包含任何 OTEL 头文件。

### 6. 统一业务字段

可以提供标准字段辅助方法：

```cpp
logger.with_request_id(id);
logger.with_trace_id(id);
logger.with_user_id(id);
logger.with_service(name);
```

## 验收标准

- 可以从上下文中自动提取请求信息
- 请求内日志可以自动关联 request ID
- 异常链不会丢失
- Fatal 和崩溃场景能够记录必要堆栈
- OpenTelemetry 集成不会强依赖具体追踪实现
- 业务字段命名统一

## 建议版本

```text
v0.5.0
```

---

# 九、M6：安全、可靠性与生产能力

## 目标

解决生产环境下最容易出问题的部分。

## 主要功能

### 1. 敏感字段脱敏

默认关注：

```text
password
passwd
token
access_token
refresh_token
secret
authorization
cookie
private_key
credit_card
id_card
```

例如：

```json
{
  "user": "tom",
  "password": "***",
  "token": "ab***yz"
}
```

支持：

- 字段名匹配
- 自定义脱敏函数

### 2. 日志注入防护

**一条记录只占一行**是格式的硬约束：下游（journald / Filebeat / Loki / `grep`）一律按 `\n` 切分，跨行记录对它们就是 N 条独立记录，且**无法区分续行与伪造行**——这个歧义在消费端修不掉。

已实现的处理：

| 来源 | 处理 |
|---|---|
| 字段值、消息参数 | 控制字符转义：`\n` / `\r` / `\t` / `\b` / `\f`，其余 → `\xNN`（含 ESC，ANSI 序列失效） |
| 反斜杠 | 一并转义为 `\\`，否则「数据里的反斜杠+n」与「转义后的换行」无法区分，转义不可逆 |
| 格式串 | 同样中和。换行是格式约束，不是程序员的表达手段（代价：无法输出真换行） |
| 截断 | UTF-8 安全，退到完整码点边界，不产出非法 UTF-8 |

落点在 `TextFormatter` 出口统一转义（JSON 侧 `json_escape` 已覆盖），保证整条记录只有结尾一个 `\n`——框架、消息、字段三个来源一视同仁，新加的代码也绕不过去。

已知边界：文本格式用空格分隔字段，值里含空格时仍可能被解析器误认为新字段；JSON 格式无此问题。

### 3. 日志长度限制

**分层预算**，且不对格式化结果做字节切（字节切会切掉行尾换行、切出非法 JSON）：

| 配置 | 默认 | 单位 | 超出时 |
|---|---:|---|---|
| `max_message_length` | 0（不限） | 字节 | 截断 + `...` |
| `max_field_length` | 0（不限） | 字节 | 截断 + `...` |
| `max_stacktrace_length` | 512 | 字节 | **按帧丢弃** + `(+N frames)` |
| `max_record_size` | 1024 | 字节 | **先丢末尾字段，再截断消息** |

要点：

- 字段上限**按目标格式渲染后度量**——同一个值在 text 与 JSON 下长度不同（如 `double` 的 NaN 是 `nan` 对 `null`）
- 消息与字段的默认值是 0（不限）：绝大多数日志远小于总预算，单独限制只在塞大 payload 时才有意义；堆栈不同，它**总是**很长，所以默认就有预算
- `max_record_size` 小于「框架 + `...`」时不可满足：框架（时间戳+级别+文件行号）必输出（切了整条记录无法解析），消息退化为 `...`

### 4. 日志聚合去重

**要解决的问题**：某条代码路径高频吐同一条消息（循环里打日志、异常风暴）。

它原本的四个后果里，**三个已由前面的里程碑解决**，不需要靠采样来处理：

| 后果 | 现状 |
|---|---|
| 磁盘被刷满 | 已由 M3 轮转 + `max_backups` 限住（总量有界） |
| 异步队列积压 / 内存上涨 | 已由 M4 `buffer_size` 上限 + `DropDebug` 限住 |
| 业务线程被拖慢 | 已由 M4 异步写入解耦 |
| **噪声吃掉有效日志** | 未解决 ← 本项要处理的 |
| 下游采集 / 存储成本 | 未解决（成本问题，另议） |

**做法：聚合而非丢弃。**

相同 `(level, file, line)` 的重复日志合并成一条，附带出现次数：

```text
TEXT: ...query failed: timeout (repeated 1000 times)
JSON: {..., "msg": "query failed: timeout", "repeated": 1000}
```

- 键用 `(level, file, line)`，**不用格式化后的消息**：级别过滤之后立刻可算，不依赖格式化——否则最贵的一步已经做完，聚合就失去意义了。而且它正好对应「同一处代码在循环里刷屏」这个真实场景
- 状态放 `thread_local`：热路径零锁。代价是计数按线程而非全局，对「防噪声淹没有效日志」够用
- 序列结束（下一条不同消息、窗口过期、或 `flush_all()`）时补发摘要

**实现要点**（`include/logger/dedup.h` + Logger 接线）：

- `DedupFilter` 只做计数与判定，不认识 Logger / Record，可单独测试
- 计数是 **`Record::repeat_count` 结构化字段**，不是往成品字符串上拼后缀——TEXT 追加到正文、JSON 输出数字成员，两个格式各自给出正确结构（往格式化结果上追加在 JSON 下会落到 `}` 之后，直接产出非法 JSON）
- 载荷是**流水线末端**（构造完 Record、路由之前）捕获的完整 Record，含上下文 / 堆栈 / 字段。以后在上游新增处理步骤，载荷自动带上，不需要改 dedup
- **首次重复**那条照常走完整组装、但只存不发，作为摘要载荷；第 3 条起直接丢弃。每个序列只多付一次组装
- 已知限制：`thread_local` 决定了 `flush_all()` 只能收**当前线程**的挂起摘要，其它线程的要等它们各自的下一条不同日志才会补发。线程打完风暴就闲置时，摘要会一直挂着

**不做固定比例采样**：

- 它丢多少取决于**比例**，聚合丢多少取决于**重要性**——对排障来说后者才对
- 而它想解决的磁盘 / 队列 / 性能问题，M3 / M4 已经解决了
- 例外：**按请求 ID / trace ID 采样**是另一回事（保证同一请求的日志要么全留要么全丢），属于下游成本优化，需要时单独设计

### 5. 高频日志的根因治理

聚合只是兜底，根因是**代码缺陷**——循环里打日志：

```cpp
for (...) { if (fail) LOG_ERROR("retry failed: {}", err); }   // 差：N 行
LOG_ERROR("retry failed {} times, last: {}", n, err);         // 好：1 行
```

库能做的是**帮用户发现它**，而不是默默兜着：

- 开发 / 测试环境：检测同一 `(level, file, line)` 在短时间内高频重复，打一条**一次性警告**，提示"这里可能在循环里打日志"
- 成本接近零（只在真的重复时触发），且把问题推回给该修代码的人
- 生产环境关闭 —— 那里靠聚合兜底

另外，高频事件本质是**指标**而不是日志：「重试失败 1000 次」该是一个 counter 定期导出，而不是 1000 行日志。见下一节的「日志自身指标」。

### 6. 日志自身指标

至少暴露：

- 写入成功数量 `written`
- 写入失败数量 `failed_writes`
- 丢弃数量 `dropped`
- 按级别统计 `by_level[]`
- 队列长度 `queue_length`
- 队列峰值 `queue_peak`
- 输出延迟 `max_write_latency_us`
- 编码失败数量 —— 并入 `dropped`（编码抛异常即丢弃该条）
- 脱敏次数 `masked_fields`
- 聚合去重数量 `dedup_suppressed`

实现要点：

- `Logger::stats()` 返回 `LogStats` 快照。计数用 `relaxed` 原子累加，**读取不加锁**；
  只有队列长度需要短暂持 `queue_mtx` —— `std::deque::size()` 与 push/pop 并发是数据竞争
- 队列峰值在**满队判断那一处**就地更新即可：后续分支大小要么不变（满）、要么 +1（未满），
  不必在每个 push/pop 点各记一次
- **异常只有两处兜底**：
  - `log_impl` 是 `noexcept` 边界，唯一的 try/catch；实际实现移入 `log_impl_inner` 允许抛。
    任何异常（`bad_alloc` / 用户 `operator<<` 或 masker 抛出 / 库自身意外）都丢弃整条并计 `dropped`
  - `async_loop` 是线程函数，未捕获的异常会 `terminate` 整个进程；兜底必须放在 `write_one`
    调用点上，**且不能让异常越过 `in_flight` 复位**，否则 `flush_all()` 会永久等待
- 因此子函数（`mask_sensitive_fields`、`limit_field_lengths`、`log_impl_sync`）不再各自
  try/catch，一律向上抛
- 唯一保留的就地恢复是 `write_to_sinks` 的 **per-sink** try：那是"这个 sink 挂了继续试下一个"，
  与前者的"整条失败即放弃"粒度不同
- 行为变更（安全）：`masker` 抛异常不再"保留原值继续输出"，而是丢弃整条 ——
  保留原值等于把敏感数据原样写进日志
- 不记录「轮转次数」：它在 Sink 里，而 `LogSink` 接口没有统计面；为一个诊断价值有限的
  数字去改接口不划算（磁盘上的文件本身就是证据）

### 7. 动态调整级别

库提供线程安全的改级接口，应用自行接到 HTTP / SIGHUP / 配置中心：

```cpp
Logger::get_instance().set_level(LogLevel::DEBUG);
```

要求：

- **只负责「改」**：持久化、鉴权、限流都是应用的职责，库不持有存储、也不知道谁在调
- 改级立即对热路径可见（级别是原子热缓存，热路径 lock-free 早退）
- **不影响已构造 / 已入队的记录**：record 持有配置快照，改级只对新记录生效（快照语义）

模块级配置（按模块设级别与独立 Sink / 格式）见 M8。

## 验收标准

- 常见敏感字段默认可脱敏
- 用户输入不能伪造日志行
- 单条日志大小可控
- 日志风暴时不会拖垮应用
- 聚合去重行为可观察
- 改级立即生效，且不影响已入队的记录
- Logger 自身有监控指标

## 建议版本

```text
v0.6.0
```

---

# 十、M7：测试、文档与正式发布

> **状态：已完成（v1.0.0，2026-09-14）**。7 个 ctest 目标（单元 + 集成 + soak + 生命周期 +
> IO 失败）、三套 sanitizer 配置（ASan+UBSan / TSan）、clang-tidy 静态检查（零告警）、
> Doxygen API 文档（零警告）、`docs/` 下 13 篇文档、16 个场景的 Google Benchmark 基准。
> 兼容性承诺自 v1.0.0 起生效，范围见 [compatibility.md](compatibility.md)。

## 目标

将项目从“功能可用”提升到“可以被团队长期依赖”。

## 主要任务

### 1. 单元测试

使用 GoogleTest + CTest，覆盖：

- 级别过滤
- 字段格式化
- JSON 编码
- 文本编码
- 多 Sink
- 文件轮转
- 异步队列 / 队列满处理
- `close()` / `flush_all()`
- 错误记录（异常链、`LOG_EXCEPTION`）
- 错误堆栈
- Context
- Trace（traceparent 编解码）
- 脱敏
- 注入防护（转义、单行不变式）
- 长度限制（消息 / 字段 / 记录三层预算）
- 聚合去重
- 自身指标
- 崩溃捕获
- 异常边界（用户 `operator<<` / masker 抛异常不拖垮进程）
- 运行期改级

### 2. 集成测试

只测本项目真实具备的对象：

- Logger + 文件系统（轮转、目录缺失、权限）
- 多线程并发（同步 / 异步）
- 进程退出刷盘（析构 / `close()`）
- 崩溃捕获 + 真实崩溃进程（子进程验证）

> OpenTelemetry / 配置中心 / HTTP 中间件 / 远程 Sink 都不在本项目范围内（见 M0 决策表），
> 没有可集成的对象，移到 M8。

### 3. 异常测试

模拟：

- 文件权限错误
- 磁盘满
- `ofstream` 写入失败
- 队列满
- 重复 `close()` / 并发 `close()`
- 用户代码抛异常（`operator<<` / 脱敏函数）
- 崩溃信号（SIGSEGV / SIGABRT）
- 持续高并发（固定时长，如 30 秒内 8 线程 × N 条，断言无丢失、无交叉、无泄漏）

### 4. 数据竞争和泄漏测试

- ThreadSanitizer（数据竞争）
- AddressSanitizer + LeakSanitizer（非法内存访问、内存泄漏；ASan 自带 LSan）
- UndefinedBehaviorSanitizer（未定义行为）
- 文件句柄泄漏（对比 `/proc/self/fd` 计数）
- 线程泄漏（进程退出时无残留可 join 线程）

> 不做 helgrind（与 TSan 重复且慢得多）、不做 Valgrind（与 ASan 互斥，二选一）、
> 不检查 socket 泄漏（库不开 socket）。
> 「锁死」不能作为独立检查项 —— 死锁靠**超时**暴露，体现在并发用例必须在限定时间内返回。

### 5. 文档完善

文档至少包括：

```text
README
快速开始
API 文档（头文件注释 + Doxygen）
配置说明
结构化日志指南
同步模式指南
异步模式指南
文件轮转指南
脱敏指南
Trace 集成指南
性能报告
故障排查
破坏性变更清单
```

> 不写「升级指南」：v1.0 之前没有对外发布过版本，没有用户需要升级；
> 改为「破坏性变更清单」，把 M2 的 printf → `{}`、M6 的字节切 → 分层预算等已发生的
> 变更记在案，供将来写升级指南时取材。

### 6. 示例项目

提供完整示例：

- 控制台日志
- JSON 日志
- 文件日志（含轮转）
- 异步日志
- Trace 日志
- 脱敏日志
- 运行期改级

### 7. 版本和兼容性

明确：

- API 稳定性（公共头文件即接口）
- ABI：**只承诺同编译器 + 同标准库下的稳定**；明确**不承诺跨标准库 ABI**
  （`std::string` / `std::vector` 等布局在 libstdc++ 与 libc++ 之间就不同，静态库跨标准库链接本就不成立）
- 宏冲突策略（统一前缀，如 `LOG_`，避免与业务宏冲突）
- 配置兼容性
- JSON 字段兼容性
- 废弃 API 处理方式（`[[deprecated]]`）
- 破坏性变更规则
- Release Note 规范

## 验收标准

- §1 列出的能力项**逐项有对应用例**（以测试项清单为准，不设覆盖率百分比门槛 ——
  覆盖率数字容易为了达标而写无断言的形式用例，不如按能力项逐条核对）
- 并发、异常、性能测试通过；并发用例均在限定时间内返回（超时即视为死锁失败）
- Sanitizer（ASan + LSan / TSan / UBSan）全绿
- 文档可执行地验证：**不读库源码、只照 README 就能跑通 example 并看到预期输出**
- 有完整变更日志
- 有可重复执行的 CI 流程（CMake + CTest），PR 与主干都跑

## 建议版本

```text
v1.0.0
```

---

# 十一、M8：后续高级能力

这些功能不建议放入第一版，但可以作为后续路线图。

## 1. 远程日志 Sink

**先做一个：HTTP**（最通用，云厂商日志服务通常也走它）。其余协议是**互斥替代**，不是都要做，按需再加：

| 协议 | 何时考虑 |
|---|---|
| HTTP | 首选 |
| TCP / UDP | 已有 syslog 或自建收集端 |
| Kafka / gRPC | 已在用对应中间件 |

实现一个网络 Sink 需要解决的公共问题：

- 网络重试与退避
- 批量发送与数据压缩
- 连接复用
- 断网恢复
- 背压：远程不可用时**不能拖垮业务**（队列满策略见 M4，输出失败降级见 M3）

## 2. 日志持久化队列

解决的是「**进程重启 / 队列满时避免丢日志**」，**与有无远程 Sink 无关**：

```text
内存队列 → 磁盘队列 → 落盘（本地文件或远程 Sink）
```

要点：

- 落盘格式与重放顺序
- 重启后的重放
- 磁盘占用上限与清理（复用 M3 保留策略的思路）

## 3. 模块级日志配置

**前置：先定义「模块」是什么**，否则本条无法实施。可选来源：

- 子 Logger 命名，如 `Logger::get_instance().with_module("database")`
- 按源文件目录推断
- 显式参数

定了来源之后，配置形如：

```text
http=info
database=debug
cache=warn
```

并可对模块配置独立 Sink 与格式。

## 4. 高级采样

**接的是 M6 留下的那个例外**：M6 §九.4 明确「按请求 ID / trace ID 采样属于下游成本优化，需要时单独设计」——本节就是那份设计。

采样键直接复用 M2 的字段（见字段命名规范）：

| 采样键 | 依据字段 |
|---|---|
| Trace ID | `trace_id` |
| 用户 | `user_id` |
| 租户 | `tenant_id` |
| 错误类型 | `error_type` |
| 请求路径 | 需业务补 `path` 字段 |

> 与 M6 的聚合去重是两件事：聚合是「防噪声淹没有效日志」；采样是「按请求整体保留或丢弃，降低下游成本」。

## 5. 按条件过滤

**不是采样**，而是「只记录满足条件的日志」：

- 只保留慢请求日志（耗时超阈值）
- 只保留特定状态码的请求

需要业务提供判定依据（耗时、状态码等字段）。

## 6. 多 Logger 实例

审计日志对完整性、权限、存储的要求属于**部署与存储层**，不是日志库的职责。

库侧能提供的是：支持多个互不干扰的 Logger 实例，各自持有 config / Sinks / 文件：

```cpp
Logger audit_logger{audit_config};   // 独立配置与输出目标
```

审计日志「不允许随意删除」「更严格的权限」由运维侧保证（独立存储、权限控制、WORM）。

## 7. 日志分析辅助

- 日志事件 ID（稳定标识，便于跨系统关联）

> 「错误聚合」「重复日志合并」已在 M6 的聚合去重里覆盖；
> 「metrics / 调用耗时统计」属于 APM，不放进日志库。

---

# 十二、项目目录结构

已落地结构如下：

```text
logger/
├── CMakeLists.txt
├── Doxyfile                          # API 文档配置（-DBUILD_DOCS=ON 后 --target docs）
├── README.md
├── include/logger/                   # 公共 API：头文件即接口承诺
│   ├── logger.h                      # Logger 单例 + LOG_* 宏 + with()
│   ├── level.h                       # 级别枚举
│   ├── config.h                      # 配置、LogStats、脱敏规则
│   ├── field.h                       # KV 字段 + encode 编码入口
│   ├── literals.h                    # 容量字面量（10_mb 等）
│   ├── version.h                     # 版本号（LOG_VERSION 等）
│   ├── context.h                     # ContextScope 作用域上下文
│   ├── stacktrace.h                  # 调用栈采集
│   ├── trace.h                       # W3C traceparent 编解码
│   ├── crash_handler.h               # 崩溃信号处理
│   ├── sink.h                        # Sink 抽象 + SinkInput
│   ├── sink/
│   │   ├── console_sink.h            # 控制台（按级别分流 stdout / stderr）
│   │   └── file_sink.h               # 文件（按日期 / 大小轮转）
│   └── detail/                       # 内部实现：不承诺稳定，且不进 API 文档
│       ├── utils.h                   # 时间 / 转义 / UTF-8 截断
│       ├── error.h                   # 异常信息提取
│       ├── dedup.h                   # 聚合去重状态机
│       ├── record.h                  # 日志记录
│       ├── format.h                  # {} 位置参数格式化
│       └── formatter/
│           ├── text_formatter.h
│           └── json_formatter.h
├── src/                              # 实现，目录结构与 include/ 一一对应
│   ├── logger.cpp  field.cpp  stacktrace.cpp  trace.cpp  crash_handler.cpp
│   ├── detail/
│   │   ├── utils.cpp
│   │   └── formatter/{text,json}_formatter.cpp
│   └── sink/{console,file}_sink.cpp
├── examples/                         # 可运行示例
├── benchmark/                        # 性能基准（Google Benchmark，可选依赖）
├── test/{unit,integration}/          # 单元与集成测试
└── docs/                             # 设计文档
```

核心原则：

- **公共 API 尽量稳定**（头文件即接口）；公共头统一在 `include/logger/` 下，
  消费者 `#include <logger/...>`，避免重名
- **公共面按「用户能不能调用」划分**：用户拿不到的（记录模型、Formatter、
  格式化与转义工具、异常提取、聚合去重状态机）一律放 `detail/`。
  它同时被 Doxyfile 的 `EXCLUDE` 排除，不出现在 API 文档里 —— 这比在 Doxyfile 里
  逐个列符号名好维护
- **`src/` 镜像 `include/` 结构**，只放非模板实现；模板实现留在头文件
- 文件名 snake_case，去掉 `log_` 前缀（目录已含 `logger`）
- Formatter 与 Sink 解耦；异步逻辑不侵入记录模型
- 脱敏、聚合去重是**可配置能力**，不是可插拔插件；不做比例采样（见 §九.4）
- 低层模块不依赖高层业务；日志代码路径 noexcept，不向业务抛异常
- 宏统一 `LOG_` 前缀，避免与业务代码宏冲突

命名上的一处偏差（已消除）：

- `__FILENAME__` 曾用双下划线前缀（属于**实现保留标识符**），M7 重构时已改为在宏里直接调用
  `filename_of(__FILE__)`，该宏不复存在
- `logger.hpp` 这类 umbrella header 未提供：公共头只有十来个，按需 include 即可
- `encode` / `KV` / `filename_of` 等符号位于**全局命名空间**，这是有意的：`encode` 是自定义
  类型的扩展点，必须在全局命名空间才能被库内的限定调用找到（见
  [兼容性说明](compatibility.md)）

---

# 十三、建议的版本规划

## v0.1.0：基础日志

包含：

- 日志级别
- 文本格式
- stdout/stderr
- 基础配置
- 并发安全

## v0.2.0：结构化日志

包含：

- Key-Value 字段
- JSON 格式
- With 字段
- 自定义字段类型

## v0.3.0：生产输出

包含：

- 文件输出
- 文件轮转
- 多 Sink
- 输出失败处理

## v0.4.0：高性能

包含：

- 异步日志
- 批量写入
- 队列满处理
- Benchmark
- 优雅关闭

## v0.5.0：上下文与错误

包含：

- Context（thread_local / RAII）
- Request ID
- Trace ID
- 异常链
- Stacktrace

## v0.6.0：安全与治理

包含：

- 脱敏
- 日志聚合去重
- 日志长度限制
- 自身指标
- 动态调级

## v1.0.0：稳定版本

**已发布（2026-09-14）**。

| 项 | 状态 |
|---|---|
| 完整测试 | ✅ 7 个 ctest 目标；单元 / 集成 / 异常 / 并发 / 生命周期 / 持续高并发 |
| 文档 | ✅ README + 8 篇指南 + 兼容性 / 性能报告 / 破坏性变更 / CHANGELOG |
| API / ABI 稳定 | ✅ 承诺范围见 [compatibility.md](compatibility.md)（明确不承诺跨标准库 ABI） |
| 性能报告 | ✅ [performance.md](performance.md)，16 个场景 |
| 兼容性承诺 | ✅ 自本版本起按语义化版本承诺 |
| 生产案例 | ❌ **无** —— 本项目未在真实业务中运行过，这一项空着 |

---

# 十四、每个里程碑的统一完成标准

每个 Milestone 不应该只看“代码写完”，建议采用以下 Definition of Done：

## 功能

- 需求已实现
- 正常场景可用
- 异常场景有明确行为
- API 有示例

## 测试

- 单元测试通过（GoogleTest + CTest）
- 集成测试通过
- 并发测试通过
- 失败场景测试通过

## 性能

- 有 benchmark（Google Benchmark）
- 没有明显性能回退
- 内存分配可接受
- 高并发下无异常

## 文档

- README 已更新
- 头文件注释已更新
- 配置项已说明
- 兼容性影响已说明

## 工程质量

- CI 通过
- 静态检查通过（clang-tidy / cppcheck）
- 数据竞争检查通过（TSan）
- Sanitizer（ASan / UBSan / LSan）通过
- 无资源泄漏
- 无未解决的高优先级问题

---

# 十五、关键风险与应对策略

## 1. 功能范围膨胀

风险：

```text
一开始就同时做远程日志、配置中心、Trace、审计、采样、云服务适配
```

应对：

- 先完成本地核心 Logger
- 先稳定 API
- 高级能力通过插件扩展
- 第一版只聚焦 stdout、文件和结构化日志

## 2. 异步日志丢失

风险：

- 程序异常退出
- 队列满
- 后台线程崩溃
- Close 没有刷盘

应对：

- 明确丢弃策略
- 提供 flush 和 close（或依赖析构）
- Error 日志支持同步降级
- 暴露丢弃指标
- 增加异常退出测试（signal / abort）
- 关键日志走同步路径或 atexit 兜底刷盘

## 3. 日志拖垮业务

风险：

- 日志量过大
- JSON 编码耗时
- 磁盘写入阻塞
- 错误风暴

应对：

- 级别过滤（宏入口处早退）
- 异步写入
- 聚合去重
- 单条日志大小限制
- 业务线程和 Sink 解耦

## 4. 敏感信息泄漏

风险：

- 自动打印上下文
- 异常对象包含密码
- 请求头被完整输出
- 用户字段未脱敏

应对：

- 默认敏感字段规则
- 明确禁止打印的字段
- 脱敏测试
- 安全审计
- 文档中明确责任边界

## 5. API 与 ABI 不稳定

风险：

- 过早公开太多内部接口
- 模板 / 头文件频繁变更触发大量重编译
- 字段命名频繁变化
- JSON 格式不兼容
- 配置项命名混乱
- 宏与业务代码冲突
- 不同编译器 / 标准库 ABI 断裂

应对：

- v0.x 阶段允许调整
- v1.0 前保留扩展空间
- 稳定核心接口，公共头文件保持最小面
- 用 PIMPL / 类型擦除隔离实现细节
- 宏统一前缀，提供关闭宏的开关
- 不要过早承诺所有内部类型

## 6. 多进程与文件轮转冲突

风险：

- fork 后子进程共享日志文件句柄
- 多进程写同一文件内容交叉
- 轮转时文件被重命名导致句柄失效

应对：

- 明确单进程单文件的默认模型
- 多进程部署时按进程号 / 主机名区分文件
- 轮转时检测句柄有效性并自动重建
- 必要时引入文件锁协调轮转

---

# 十六、最终成功标准

一个 C++ Logger 项目达到生产级，至少应该满足：

1. API 简单，业务开发者容易使用（宏 + 结构化接口）。
2. 默认配置安全，不容易泄漏敏感信息。
3. 日志级别过滤有效，关闭日志时开销足够低。
4. 支持结构化日志，能够被机器检索。
5. 支持 stdout、文件和自定义输出。
6. 高并发下不会出现数据竞争、死锁和资源泄漏。
7. 异步模式下有明确的背压和丢弃策略。
8. 程序退出时能够正确刷盘（RAII 或显式 close）。
9. 输出失败时不会轻易拖垮主业务。
10. 支持 request ID、trace ID 和错误堆栈。
11. 具备日志聚合去重与大小控制。
12. Logger 自身的异常和丢弃行为可观测。
13. 有完整测试、Benchmark 和文档。
14. v1.0 之后能够保持 API、ABI 和日志格式兼容。
