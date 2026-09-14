# Changelog

本文件记录本项目的所有重要变更。格式遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
版本号遵循[语义化版本](https://semver.org/lang/zh-CN/)。

**自 v1.0.0 起按语义化版本承诺兼容性**（承诺范围见 [docs/compatibility.md](docs/compatibility.md)）；
`0.x` 期间的历次破坏性变更记录在 [docs/breaking-changes.md](docs/breaking-changes.md)。

## [1.0.0] - 2026-09-14

M7：测试、文档与正式发布。API 稳定，自本版本起按语义化版本承诺兼容性。

### Added

- 完整文档：重写 README，新增 `docs/guides/`（结构化日志、上下文、错误与堆栈、Trace、脱敏、
  同步与异步、文件轮转、故障排查）与 `docs/{compatibility,performance,breaking-changes}.md`
- 5 个可运行示例：文件轮转、异步、Trace、脱敏、运行期改级
- 集成测试：`test_io_failure`（权限、目录消失、fd 泄漏）、`test_lifecycle`（并发 close、
  重复 close、线程回收）、`test_soak`（固定时长高并发，`LOGGER_SOAK_SECONDS` 可调）
- 单元测试：上下文、异常提取、堆栈、traceparent、脱敏、注入防护、长度预算、聚合去重、自身指标
- 崩溃处理器：子进程真实崩溃的自动化断言（信号名、故障地址、出错指令）
- Doxygen API 文档（`BUILD_DOCS=ON` + `docs` 目标），`WARN_AS_ERROR=YES` 零警告
- `LogStats::queue_peak`、`LogStats::max_write_latency_us`
- [性能报告](docs/performance.md)：16 个场景的实测吞吐与 P50/P95/P99，含 M5 / M6 新增能力的开销
- **版本宏** [version.h](include/logger/version.h)：`LOG_VERSION` / `LOG_VERSION_MAJOR` /
  `LOG_VERSION_MINOR` / `LOG_VERSION_PATCH` / `LOG_VERSION_CODE`。配套的 `test_version`
  会比对 `CMakeLists.txt` 的 `project(VERSION)`，两处版本号漂移会直接测失败
- **clang-tidy 静态检查**（[.clang-tidy](.clang-tidy) + CI job）：`bugprone-*` / `clang-analyzer-*` /
  `performance-*` / `modernize-use-override`，含头文件模板，保持零告警（CI 里 warning 即失败）

### Changed

- **性能基准改用 Google Benchmark**（可选依赖，`apt install libbenchmark-dev`；未安装则跳过基准目标），
  场景从 7 个扩到 16 个：补上上下文、脱敏、去重、长度预算、转义、异常、堆栈采集/符号化、
  队列满丢弃等此前从未测过的路径。相比自研 harness 补上了自适应迭代、多轮重复与变异系数、
  编译屏障（`DoNotOptimize`）、JSON 输出
- **内部实现移入 `include/logger/detail/`**，Doxyfile 按目录排除；`EXTRACT_ALL=NO` 让
  `WARN_IF_UNDOCUMENTED` 真正生效
- `FormatResult` → `SinkInput`（它是 Sink 的输入，不是"格式化结果"）
- `include/logger/utiils.h` → `include/logger/detail/utils.h`；`src/` 目录结构与 `include/` 对齐
- `Logger` 的构造函数全部私有，拷贝赋值与移动赋值禁用；只能由 `get_instance()` 或 `with()` 得到实例
- `KV` 的 key 长度改按 `strlen` 取（原来按数组长度取，非常量数组会产生带 NUL 的字段名）

### Fixed

- **`with()` 得到的子 Logger 析构会关闭整个日志器**：它的析构等价于 `close()`，
  而它与单例共享状态。异步模式下后果是后台线程被 join、后续日志入队后无人消费 ——
  `flush_all()` 永久挂起，记录静默丢失（`dropped` 还是 0）。
  现在只有状态所有者（单例）的析构会关闭日志器
- **`close()` 之后继续打日志会入队但无人消费**：`Block` 策略下等一个永远不会到来的空间，
  直接把调用方卡死。现在改为丢弃并计入 `dropped`（符合「关闭即停止接收新日志」的约定），
  同时唤醒仍阻塞在队列满上的生产者
- 枚举类型传给 `KV` 时的报错不可读（几十行模板候选列表）→ 改为一条 `static_assert` 提示
- **`KV("err", e)` 丢掉异常信息**：`encode(std::exception)` 的参数是**按值**传的，
  派生异常被切片成 `std::exception`，`what()` 退化成基类的 `"std::exception"` ——
  `KV("err", std::runtime_error("disk full"))` 输出的是 `std::exception` 而不是 `disk full`。
  改为按引用接收（`LOG_EXCEPTION` 走的是另一条路径，一直是对的）
- **宏入口每条日志多花约 350 ns**：文本格式化器渲染 `[thread_id][file:line][func]` 时构造
  `std::ostringstream`（`std::thread::id` 只能经 `operator<<` 输出）。改为按线程 id 缓存文本
  （`thread_id_str()`），并把拼接缓冲的 `reserve` 补上。文本路径整体快约 20%
  （`SyncText` 1173 → 912 ns），输出格式一字未变

## [0.6.0] - 2026-09-13

M6：安全、可靠性与生产能力。

### Added

- **脱敏**：`enable_sensitive_field_mask` 开关、`sensitive_field_masker` 自定义函数、
  `is_sensitive_key()` 关键词表（password / token / secret / authorization / private_key 等 11 个，
  精确匹配）。脱敏发生在**编码之前**，masker 拿到的是未编码的原始值；
  只有值真的改变才重建字段，未命中的字段类型原样保留
- **日志注入防护**：统一单行不变式 —— 一条记录永远只占一行。文本格式做**可逆**转义
  （反斜杠一并转义，换行、控制字符转成转义序列），JSON 按规范转义
- **长度限制**分三层：`max_message_length`（消息正文）、`max_field_length`（单个字段值）、
  `max_record_size`（整条记录预算）。超预算时按「先丢尾部字段 → 再截断消息 → 最少形态」降级；
  截断按 UTF-8 字符边界，不会切出半个字符
- **聚合去重** `dedup_window_ms`：同一线程内相同 `(level, file, line)` 只输出首条，
  序列结束时补一条带重复次数的摘要（JSON 下是独立的 `repeated` 成员）。
  判定早于字段合并与堆栈采集，重复日志这些开销全部省掉
- **自身指标**扩展到 10 个字段：除原有的写入/失败/丢弃外，新增 `by_level[7]`（按级别）、
  `queue_length` / `queue_peak`（队列水位）、`max_write_latency_us`（打点到写完的最长延迟）、
  `masked_fields`、`dedup_suppressed`
- **运行期改级** `set_level()`：只改级别、不动其它配置，线程安全且立即对热路径生效
- **崩溃现场捕获** `install_crash_handler()`：signal handler 里只用 async-signal-safe 接口
  （自实现的数字格式化，不用 printf / std::string / Logger），记录 pid、tid、信号与 `si_code`、
  故障地址、出错指令指针（PC / RSP / RBP）、image base、栈回溯原始地址。
  使用独立信号栈（栈溢出也能记录），写完恢复默认处理并重抛信号以**保留 core dump 语义**；
  某信号已有非默认 handler（ASan / JVM 等）时跳过不接管，卸载时精确还原
- `config.stacktrace`（`StackTraceMode`：`OFF` / `FATAL` 默认 / `ALWAYS`）与 `stacktrace_depth`

### Changed

- **`max_log_item_size`（对格式化结果做字节硬切）→ `max_record_size`（分层预算）**。
  迁移：把 `cfg.max_log_item_size = N` 换成 `cfg.max_record_size = N`；若要单独限制正文或字段，
  再用新增的 `max_message_length` / `max_field_length`。
  硬切会切掉行尾换行、切出非法 JSON，分层预算不会
- 无嵌套异常时不再输出 `error_chain` 字段（空 key 的字段在入库时被跳过）
- FATAL 级日志默认自动附上调用栈（`config.stacktrace = StackTraceMode::FATAL`）
- M5 设计里计划的比例采样改为聚合去重（采样只在设计阶段存在，未实现过）：
  采样会连"首次发生"一起丢掉，去重保留首条与总次数，对"防止噪声淹没有效日志"更合适

## [0.5.0] - 2026-09-12

M5：上下文、错误与链路追踪。

### Added

- **`ContextScope`**：`thread_local` 栈 + RAII 的作用域上下文，进入作用域后本线程所有日志
  自动带字段，出栈自动清理（异常路径同样成立）。支持嵌套，内层覆盖外层同名 key
- **字段预绑定 helper**：`with_request_id` / `with_trace_id` / `with_span_id` / `with_user_id` /
  `with_service` / `with_trace`
- **`LOG_EXCEPTION`** 与 `Logger::exception()`：自动展开为 `error`（最内层 `what()`，即根因）、
  `error_type`（demangle 后）、`error_chain`（caused-by 链，仅嵌套时输出）。
  剥掉 `std::_Nested_exception<T>` 这类实现内部包装，只留业务类型名；支持 `std::exception_ptr`
  以便跨线程传递
- **`StackTrace`**：采集与符号化分离 —— `capture()` 只取返回地址（快、不分配），
  `str()` 才 `dladdr` + demangle 并缓存。超出预算按帧丢弃并附 `(+N frames)`，不做字节硬切
- **`TraceContext`**：W3C traceparent 编解码。`parse_traceparent()` 宽容（格式不合法按"无 trace"
  处理），`make_traceparent()` 严格（任一字段非法返回空串，不发出语义被改过的头）；
  `sampled()` 取 `trace_flags` 的 bit0 而不是"整字节非 0"

### Changed

- 明确字段优先级：**调用点 KV > `ContextScope` > `with()` 预绑定**（越靠近调用点越优先）

## [0.4.0] - 2026-09-09

M4：异步、并发与高性能。

### Added

- **异步模式** `config.async`：惰性启动后台线程，业务线程只构造记录并入队即返回；
  字段在入队前物化，脱敏在入队前完成，格式化与符号化在后台线程
- **队列满策略** `asy_que_ful_strategy`：`Block`（阻塞，尽量不丢）/ `DropNewest` /
  `DropOldest` / `DropDebug`（按级别从低到高牺牲）
- **优雅关闭**：`close()` 停止接收、等队列消费完、刷盘、回收线程，返回统计快照；
  可重复、可并发调用（`std::call_once`）。析构自动调用
- `flush_all()`：等异步队列清空并写完，再刷新 Sink
- 性能优化：级别过滤走原子热缓存（关闭级别接近普通函数调用）、计数用 relaxed 原子、
  仅队列长度需要短暂持锁
- 零依赖自研 benchmark（吞吐 + P50/P95/P99），覆盖同步/异步/JSON/字段/并发/文件六个场景

### Changed

- 开发期一度引入的 `AsyQueFulStrategy::SyncFallback` 被移除（队列满时改同步写入会让
  业务线程的延迟特性随队列状态突变，难以预期）。该策略未出现在任何发布版本中

## [0.3.0] - 2026-09-08

M3：生产输出。

### Added

- **多 Sink**：一条日志依次写入所有已注册的 Sink；`LogSink` 抽象可自定义接入任意后端
- **`FileSink`**：按日期（`date_interval_h`）/ 大小（`max_file_size`）轮转，
  `max_backups` 限制保留文件数；目录缺失自动创建，文件被外部删除后自动重开；
  大小判定用逻辑写入字节数，避免 `ofstream` 缓冲导致轮转滞后
- **写失败策略** `log_fail_strategy`：`FallbackToStderr`（降级到 stderr）/ `Drop`（丢弃并计数）；
  Sink 抛异常也按写失败处理，不让日志拖垮进程
- `LogStats` 起步：`written` / `failed_writes` / `dropped`

## [0.2.0] - 2026-09-06

M2：结构化日志。

### Added

- **结构化字段** `KV("key", value)`：文本输出 `key=value`，JSON 输出带类型成员；
  支持字符串、各宽度整数、浮点（含 nan/inf）、布尔、`char`、`std::vector`、时间点、
  带 `operator<<` 的自定义类型
- **JSON Formatter**（`config.format = LogFormat::JSON`）：手写编码器，保留字段类型，
  nan / inf 按规范写成 `null`
- **`with()` 预绑定字段**：返回共享 Sinks / 配置的子 Logger，可链式叠加
- 字段去重：同 key 后写覆盖（last-write-wins），保留首次出现的位置
- 空 key 处理：字面量 key 编译期 `static_assert` 拦截，运行期动态 key 入库时跳过
- 特殊字符转义（引号、控制字符）

### Changed

- **格式化风格从 printf 改为 `{}` 位置参数**。迁移：`LOG_INFO("user %d login", id)` →
  `LOG_INFO("user {} login", id)`。
  理由：`%d` 与实参类型不匹配是未定义行为且编译器难以全量检查；`{}` 由自研格式化器按实参类型
  编码，类型安全，也不依赖 iostream

## [0.1.0] - 2026-09-04

M1：基础日志。

### Added

- `Logger` 单例（`get_instance()`）与七个级别（`TRACE` / `DEBUG` / `INFO` / `WARN` / `ERROR` /
  `FATAL` / `OFF`）
- 宏入口 `LOG_TRACE` … `LOG_FATAL`，自动携带文件、行号、函数名
- 控制台输出按级别分流：`Error` 及以上 → stderr，其余 → stdout
- 文本格式：时间戳（ISO8601，本地/UTC 可选）+ 级别 + 线程 id + 文件:行 + 函数 + 正文
- 全局级别过滤 `config.log_level`，低于级别的日志在入口处丢弃
- 多线程安全（互斥锁保护），超长日志自动截断
- `LogConfig` 配置结构

[Unreleased]: https://github.com/Bennett-LiBingHai/logger/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/Bennett-LiBingHai/logger/compare/v0.6.0...v1.0.0
[0.6.0]: https://github.com/Bennett-LiBingHai/logger/compare/v0.5.0...v0.6.0
[0.5.0]: https://github.com/Bennett-LiBingHai/logger/compare/v0.4.0...v0.5.0
[0.4.0]: https://github.com/Bennett-LiBingHai/logger/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/Bennett-LiBingHai/logger/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/Bennett-LiBingHai/logger/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/Bennett-LiBingHai/logger/releases/tag/v0.1.0
