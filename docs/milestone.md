
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
- 支持日志采样、限流和丢弃策略
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
    virtual bool log(const FormatResult& result) = 0;  // 返回是否写入成功
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
| SyncFallback | 队列满时改为同步写入 |

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

支持：

```cpp
LOG_ERROR("database query failed: {}", e.what());

Logger::get_instance().error("database query failed",
    KV("err", e),
);
```

自动记录：

- error message
- error type（`typeid(e).name()` 或 `std::error_code` 类别）
- 嵌套异常链（`std::nested_exception` / `std::throw_with_nested`）
- stacktrace
- wrapped error 信息

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

### 5. Trace 系统集成

支持接入 OpenTelemetry C++ SDK（或自实现 W3C traceparent）：

```text
trace_id
span_id
trace_flags
```

日志输出示例：

```json
{
  "level": "error",
  "msg": "payment failed",
  "trace_id": "abc123",
  "span_id": "def456",
  "error": "timeout"
}
```

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
- 正则脱敏
- 自定义脱敏函数
- 部分保留
- 完全隐藏

### 2. 日志注入防护

处理用户输入中的：

- 换行符
- 回车符
- ANSI 转义字符
- 控制字符
- 超长字符串

### 3. 日志长度限制

配置：

```text
max_message_length
max_field_length
max_record_size
max_stacktrace_length
```

避免单条日志无限膨胀（当前 `MAX_LOG_CONTENT_SIZE` 已有基础，可细化为按字段/记录分层限制）。

### 4. 采样

高频日志可以进行采样：

```text
相同消息 1 秒内最多输出 100 条
重复日志只保留第一条和计数
```

常见采样策略：

- 固定比例采样
- 每秒限量
- 首条保留
- 尾条保留
- 按请求 ID 采样
- Error 不采样，Debug 采样

### 5. 限流

防止异常风暴：

```text
同一错误 10 秒内最多输出 100 条
```

同时记录：

```text
suppressed_count=1000
```

### 6. 日志自身指标

至少暴露：

- 写入成功数量
- 写入失败数量
- 丢弃数量
- 按级别统计
- 队列长度
- 队列峰值
- 输出延迟
- 编码失败数量
- 轮转次数
- 脱敏次数
- 采样数量

### 7. 动态调整级别

支持运行时变更：

```text
GET /debug/log-level
PUT /debug/log-level?module=database&level=debug
```

或者通过配置文件 + SIGHUP 重载、配置中心动态修改。

需要防止：

- 未授权访问
- 频繁修改
- 修改后配置不一致
- 重启后丢失配置

## 验收标准

- 常见敏感字段默认可脱敏
- 用户输入不能伪造日志行
- 单条日志大小可控
- 日志风暴时不会拖垮应用
- 丢弃和限流行为可观察
- 动态调级安全可控
- Logger 自身有监控指标

## 建议版本

```text
v0.6.0
```

---

# 十、M7：测试、文档与正式发布

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
- 异步队列
- 队列满处理
- Close 和 Sync
- 错误堆栈
- Context
- 脱敏
- 采样
- 限流
- 动态配置

### 2. 集成测试

测试：

- Logger + 文件系统
- Logger + OpenTelemetry
- Logger + 配置中心
- Logger + HTTP 中间件
- Logger + 远程 Sink
- 多线程并发
- 进程退出刷盘（atexit / 析构）

### 3. 异常测试

模拟：

- 文件权限错误
- 磁盘满
- ofstream 写入失败
- 队列满
- 输出连接断开
- 重复 Close
- 并发 Close
- 未捕获异常
- 崩溃信号（SIGSEGV / SIGABRT）
- 长时间高并发

### 4. 数据竞争和泄漏测试

使用 Sanitizer 和工具链检查：

- ThreadSanitizer（数据竞争）
- AddressSanitizer（非法内存访问）
- LeakSanitizer / Valgrind（内存泄漏）
- UndefinedBehaviorSanitizer（未定义行为）
- helgrind（锁竞争）
- 线程泄漏
- 锁死
- 文件句柄泄漏（fd 泄漏）
- socket 泄漏

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
升级指南
```

### 6. 示例项目

提供完整示例：

- 控制台日志
- JSON 日志
- 文件日志
- 异步日志
- HTTP 请求日志
- Trace 日志
- 脱敏日志
- 动态调整级别

### 7. 版本和兼容性

明确：

- API 稳定性（公共头文件即接口）
- ABI 兼容性（编译器 / 标准库 ABI，libstdc++ vs libc++）
- 宏冲突策略（统一前缀，如 `LOG_`，避免与业务宏冲突）
- 配置兼容性
- JSON 字段兼容性
- 废弃 API 处理方式（`[[deprecated]]`）
- 破坏性变更规则
- Release Note 规范

## 验收标准

- 核心模块测试覆盖率达到目标
- 并发、异常、性能测试通过
- Sanitizer（ASan / TSan / UBSan）全绿
- 文档能够指导新用户完成接入
- 有完整变更日志
- 有可重复执行的 CI 流程（CMake + CTest）

## 建议版本

```text
v1.0.0
```

---

# 十一、M8：后续高级能力

这些功能不建议放入第一版，但可以作为后续路线图。

## 1. 远程日志 Sink

支持：

- HTTP
- Kafka
- TCP
- UDP
- gRPC
- 云厂商日志服务

需要额外解决：

- 网络重试
- 批量发送
- 连接池
- 本地缓存
- 断网恢复
- 数据压缩
- 背压
- 远程服务不可用

## 2. 日志持久化队列

异步日志可以增加本地磁盘队列：

```text
内存队列 → 磁盘队列 → 远程日志服务
```

用于降低进程重启或网络中断造成的日志丢失。

## 3. 模块级日志配置

支持：

```text
http=info
database=debug
cache=warn
```

并且能够对模块配置独立 Sink 和格式。

## 4. 高级采样

支持：

- 按错误类型采样
- 按请求路径采样
- 按 Trace ID 采样
- 按用户采样
- 按租户采样
- 只保留慢请求日志

## 5. 审计日志

审计日志与普通运行日志建议分离：

```text
普通日志：关注系统运行
审计日志：关注用户和权限行为
```

审计日志需要：

- 更严格的完整性
- 不允许随意删除
- 更严格的权限
- 单独存储
- 更严格的字段规范

## 6. 日志分析辅助

可以增加：

- 日志事件 ID
- 错误聚合
- 重复日志合并
- 调用耗时统计
- 自动生成 metrics
- 慢请求自动记录

---

# 十二、推荐的项目目录结构

如果不考虑现有平铺结构，从零设计的理想命名与结构如下：

```text
logger/
├── CMakeLists.txt
├── include/
│   └── logger/                       # 库命名空间目录，避免头文件重名
│       ├── logger.hpp                # umbrella header（聚合所有公共头，可选）
│       ├── logger.h                  # Logger 核心 + LOG_* 宏
│       ├── level.h                   # Level 枚举
│       ├── record.h                  # 日志记录（原 LogMessage）
│       ├── field.h                   # KV 字段
│       ├── formatter.h               # 格式化接口（抽象）
│       ├── sink.h                    # Sink 抽象接口
│       ├── config.h                  # 配置
│       ├── literals.h                # 字面量
│       ├── context.h                 # ContextScope 上下文
│       ├── error.h                   # 错误与堆栈
│       ├── redact.h                  # 脱敏
│       ├── sampler.h                 # 采样
│       ├── metrics.h                 # 指标
│       ├── detail/                   # 内部实现，不进公共 API 承诺
│       │   ├── buffer.h
│       │   ├── clock.h
│       │   └── queue.h
│       ├── formatter/
│       │   ├── text_formatter.h
│       │   └── json_formatter.h
│       └── sink/
│           ├── console_sink.h
│           ├── file_sink.h
│           ├── rotating_file_sink.h
│           ├── multi_sink.h
│           └── async_sink.h
├── src/                              # 镜像 include 结构，只放非模板实现
│   ├── logger.cpp
│   ├── formatter.cpp
│   ├── formatter/
│   │   ├── text_formatter.cpp
│   │   └── json_formatter.cpp
│   └── sink/
│       └── ...
├── examples/
├── benchmarks/
├── test/
│   ├── unit/
│   └── integration/
└── docs/
```

核心原则是：

- 公共 API 尽量稳定（头文件即接口）
- 公共头文件统一放在 `include/logger/` 下，消费者 `#include <logger/...>`，避免重名
- 文件与类名去掉 `log_` 前缀（目录已含 `logger`），文件名统一 snake_case
- 模板实现放头文件，非模板实现放 src/ 编译进库
- Formatter 和 Sink 解耦
- 异步逻辑不要侵入核心记录模型
- 脱敏、采样、限流作为可插拔能力
- 内部辅助放 `detail/`（`namespace logger::detail`），不承诺稳定
- 低层模块尽量不依赖高层业务
- 日志代码路径 noexcept，不向业务抛异常
- 宏统一 `LOGGER_` 前缀，避免与业务代码宏冲突

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
- 日志限流
- 日志采样
- 日志长度限制
- 自身指标
- 动态调级

## v1.0.0：稳定版本

包含：

- 完整测试
- 文档
- API / ABI 稳定
- 性能报告
- 生产案例
- 兼容性承诺

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
- 采样和限流
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
11. 具备采样、限流和日志大小控制。
12. Logger 自身的异常和丢弃行为可观测。
13. 有完整测试、Benchmark 和文档。
14. v1.0 之后能够保持 API、ABI 和日志格式兼容。
