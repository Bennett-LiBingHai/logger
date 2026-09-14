# 故障排查

按现象查。每条都给「先看什么 → 常见原因 → 怎么确认」。

## 一条日志都没有

**先看**：有没有 `add_sink`。库**默认没有任何输出目标** —— 不挂 Sink 就是什么都不打。

```cpp
Logger::get_instance().add_sink(std::make_shared<ConsoleSink>());   // 忘了这行就什么都没有
```

按顺序排查：

| 原因 | 确认方式 |
|---|---|
| 没挂 Sink | 上面那行；`stats().written` 恒为 0 |
| 级别被过滤 | `get_config().log_level`；`OFF` 会全关 |
| 异步没刷就退出 | `main` 结束前 `flush_all()`，或依赖 `close()`（析构会自动调） |
| 日志跑到 stderr 去了 | ConsoleSink 把 `Error` 及以上送 stderr，`2>/dev/null` 会吞掉 |
| 写失败降级到 stderr | `stats().failed_writes > 0` |
| 进程在静态初始化阶段就崩了 | 见下面「崩溃」一节 |

`set_config` 是**整份替换**：先 `get_config()` 改一个字段再 set 回去，否则其余字段会被重置成默认值（比如把 `log_level` 从 `INFO` 悄悄变回 `TRACE`）。

## 日志少了 / 丢了

```cpp
const LogStats s = Logger::get_instance().stats();
// s.dropped / s.dedup_suppressed / s.failed_writes —— 三个方向
```

| 计数变了 | 原因 | 处理 |
|---|---|---|
| `dropped` | 队列满（按 `asy_que_ful_strategy` 丢） | 加大 `buffer_size`，或改用 `Block`；看 `queue_peak` 是否常顶到上限 |
| `dropped` | 写失败且策略是 `Drop` | 改 `FallbackToStderr`，或修磁盘/权限 |
| `dropped` | 编码抛异常（用户 `operator<<`、masker 抛出） | 给自定义类型的 `operator<<` 和 masker 加 `try/catch` |
| `dropped` | `close()` 之后又打了日志 | 见下面「关闭之后」 |
| `dedup_suppressed` | 聚合去重抑制了重复 | 预期行为；`dedup_window_ms = 0` 可关 |
| `failed_writes` | Sink 写失败 | 文件权限、磁盘满、路径不存在 |

**异步丢日志还有一个隐蔽来源**：业务线程入队成功、进程随后被 `kill -9`，队列里的记录没写出去。
要「一条不丢」就用同步模式，或在关键路径后显式 `flush_all()`。

### 关闭之后

`close()` 是终态，之后再打日志会被**丢弃并计入 `dropped`**（不是静默消失，也不是写进已关的 Sink）。
程序退出阶段的日志要放在 `close()` 之前，或干脆不显式调用 `close()`（单例析构会处理）。

### 日志文件不见了

- `max_backups` 到了上限：`FileSink` 按最后修改时间删最旧的，**删的是目录里所有普通文件**，
  不只 `.log`。别把 pid/锁文件放同一个目录
- 外部 `logrotate` 改名了：下次写入会自动重开新文件，但改名后的那份不再被这个进程写
- 目录被删：`FileSink` 只在构造时创建目录，运行期目录消失后会一直写失败并计入 `failed_writes`

## 输出内容不对

| 现象 | 原因 |
|---|---|
| `{}` 原样出现在日志里 | 位置参数不够：多余的占位符原样保留，不报错也不替换 |
| 数字变成带引号的字符串 | 命中了脱敏（`masked_fields` 会增加）；masker 重建字段后类型变成字符串 |
| 消息被截断，末尾有 `...` | `max_record_size` / `max_message_length` 预算触发，属预期 |
| 字段少了一部分 | 同上，超预算时**先丢尾部字段**再截消息 |
| JSON 里有重复键 | 字段名与框架保留名撞了（`time` / `level` / `msg` / `repeated` / `thread_id` / `file` / `line` / `func`） |
| 一条日志跨了多行 | 不该发生 —— 见下面「日志注入」 |

## 日志注入

**不变式：一条记录永远只占一行。** 换行、回车、制表符、`\0`、`0x7F` 及其它控制字符在渲染时转义，
反斜杠本身也转义，所以转换是**可逆**的：

```cpp
LOG_INFO("input: {}", user_input);        // user_input = "ok\n2026-01-01 [ERROR] fake"
// 文本输出：input: ok\n2026-01-01 [ERROR] fake      ← \n 是两个字符，不是真换行
// JSON 输出："msg": "input: ok\\n2026-01-01 [ERROR] fake"
```

想还原原始输入就反向解转义（`\n` → 换行，`\\` → `\`）。这样用户数据无法伪造出一条日志，
采集器也不必担心一条记录被拆成两行。

> 为什么不做多行输出：多行日志会让"一条记录"与"一行文本"不再一一对应，
> 采集器、grep、`wc -l` 全都得写成有状态解析。真要换行，就写 `\n` 显式表达。

## 卡住 / 挂起

| 现象 | 原因 | 处理 |
|---|---|---|
| 业务线程偶尔停顿 | `Block` 策略下队列满，生产者等空间 | 加大 `buffer_size`，或换 `DropDebug`；看 `queue_peak` |
| `flush_all()` 一直不返回 | 队列里有记录但没人消费 | 只在 `close()` 之后可能发生（旧版本缺陷，已修） |
| 退出时挂住 | 静态析构顺序：Logger 析构时后台线程还在写 | 在 `main` 结束前显式 `close()` |
| 加锁顺序死锁 | 库内只有一个 `impl_->mtx` 与队列锁，不交叉 | 用户 code 不要在自己的锁里调用可能阻塞的 sink 写 |

异步 + `Block` 的本质是**把日志的背压传给业务线程**。延迟敏感的服务用 `DropDebug`，
让低级别日志先牺牲。

## 崩溃

### 崩溃日志在哪

`install_crash_handler(path)` 的 `path`；传空串写 stderr。没装的话只会看到 core dump 或
系统的 "Segmentation fault"。

### 装了但没输出

| 原因 | 确认 |
|---|---|
| 该信号上已有别的 handler（ASan / TSan / JVM） | 安装时 stderr 会提示"跳过"；这类 handler 不能抢 |
| 崩溃发生在安装之前 | `install_crash_handler()` 要在 `main` 早期调用 |
| 输出文件不可写 | 写失败会退到 stderr |

### 崩溃日志怎么读

```text
signal: SIGSEGV(11)  si_code: 1 (SEGV_MAPERR)  fault_addr: 0x0
pid: 12345  tid: 12346
PC: 0x55d1f0a1b2c3  RSP: ...  RBP: ...
#0 0x55d1f0a1b2c3  0x1a2c   ./myapp
#1 ...
```

- `fault_addr` = 0x0 + `SEGV_MAPERR` → 空指针解引用
- 每帧的**偏移量已经减掉 load bias**，直接喂给 `addr2line`：

```bash
addr2line -f -C -e ./myapp 0x1a2c
```

- 栈顶是 `??` 或只有模块名：`-rdynamic` 没加到**最终可执行文件**（CMake 里由 `INTERFACE` 传递，
  但手工编译时要自己加），或者符号被 strip 了

### 崩溃日志写了但进程没退出

写完现场后会恢复默认处理并**重抛信号**，保留 core dump 语义，所以进程照常终止。
若看到日志后进程继续跑（或反复打印），说明崩溃发生在 handler 内部 —— 那会直接走默认动作，不递归。

## 性能异常

先看两个指标：

```cpp
const LogStats s = Logger::get_instance().stats();
// s.max_write_latency_us —— 从打点到写完的最长延迟，突然变大 = 磁盘抖动或 Sink 慢
// s.queue_peak           —— 逼近 buffer_size = 容量不够或写出跟不上
```

常见原因（数字见[性能报告](../performance.md)）：

| 现象 | 原因 |
|---|---|
| 每条日志约 10 µs | FATAL 默认自动采集堆栈（含符号化）。`StackTraceMode::OFF` 可关 |
| 每条日志约 5 µs | `LOG_EXCEPTION` 的嵌套链提取（demangle + 逐层展开），只在异常路径该付 |
| 每条日志约 8 µs 且消息很长 | `max_record_size` 预算触发，每条要重排多次格式。调大预算或缩短字段 |
| 同步模式打满了仍然慢 | 同步写路径全程持锁，多线程扩展性有限（4 线程约 2.2×） |
| 关掉的级别仍然有开销 | 应该只有 3 ns 左右；若明显更高，检查是不是每次都构造了 `KV(...)` 实参（实参在过滤之前求值） |

> 最后一条要留意：**级别过滤发生在宏内部，但参数表达式在调用前就已求值**。
> `LOG_DEBUG("dump {}", expensive_to_string(x))` 即使 DEBUG 关着也会付 `expensive_to_string` 的代价。
> 用 `if (level_enabled)` 或把昂贵计算放进 sink 侧。

## 相关

- [同步与异步指南](sync-and-async.md) — 队列满、关闭语义
- [文件轮转指南](file-rotation.md) — 文件与保留策略
- [性能报告](../performance.md) — 各项开销的实测数字
- [错误与堆栈指南](errors-and-stacktrace.md) — 崩溃现场与离线符号化
