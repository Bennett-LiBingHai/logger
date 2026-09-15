# 同步与异步指南

> 下文示例假定文件顶部有 `#include <logger/logger.h>`（统一入口，其余公共头都由它带进来）
> 和 `using namespace logger;`（库的公共符号都在 `logger::` 下，只有 `LOG_*` 宏在全局）。
> 示例里出现的 `#include <logger/xxx.h>` 只是为了指明那个能力定义在哪个头，
> 实际只需包含统一入口 `logger/logger.h`。


默认是**同步**：`LOG_INFO(...)` 返回时，日志已经走完格式化并写进了所有 Sink。
开启异步后，业务线程只把记录压进内存队列就返回，格式化与写盘由后台线程做。

```cpp
LogConfig cfg;
cfg.async = true;                                // 默认 false
cfg.buffer_size = 10000;                         // 队列最大长度
cfg.asy_que_ful_strategy = AsyQueFulStrategy::Block;
Logger::get_instance().set_config(cfg);          // 惰性启动后台线程
```

## 该选哪个

| | 同步 | 异步 |
|---|---|---|
| API 返回时 | 已写完 | 已入队 |
| 业务线程开销 | 格式化 + 写盘 | 构造记录 + 入队（+ 偶尔阻塞） |
| 日志丢失风险 | 无（写失败有兜底） | 队列满时按策略丢 |
| 进程崩溃 | 已写出的都在 | 队列里的内容丢失 |
| 适用 | 低频、调试期、要求不丢 | 高并发高吞吐、可容忍少量丢失 |

判断标准很简单：**业务线程的延迟是否受日志影响**。测法是看
`stats().max_write_latency_us` 和单次 `LOG_*` 的耗时（benchmark 里同步文本约 1.3 µs、
异步入队约 0.9 µs）。差得不多是因为同步也没做 fsync；真正的差距在写盘抖动时——
磁盘一卡，同步模式的业务线程跟着卡，异步的不会。

## 异步的内部行为

1. 业务线程构造 `Record`：字段已按值拷贝、消息已插值、脱敏已完成、堆栈已采集
2. 压入 `std::deque`，`notify` 后台线程，返回
3. 后台线程取出，快照一份 config / sinks，格式化并写 Sink，更新指标

由此产生几个可以依赖的性质：

- **字段值在入队前就物化了**，后台线程不会读到已经析构的调用点对象
- **脱敏在入队前完成**，队列里不存明文
- **格式化的开销在后台线程**（包括堆栈符号化），业务线程只付采集
- **配置是逐条的**：改级别 / 改格式后，已经入队的记录仍按旧配置输出

## 队列满的策略

`buffer_size` 是队列长度上限。满了之后：

| 策略 | 行为 | 丢哪条 |
|---|---|---|
| `Block`（默认） | 阻塞调用方，等后台线程腾出空间 | 不丢（代价是业务线程停等） |
| `DropNewest` | 丢掉新来的这条 | 新日志 |
| `DropOldest` | 踢掉队首（最旧的），压入新日志 | 最旧的日志 |
| `DropDebug` | 从队尾往前找级别最低的一条踢掉 | 低级别日志 |

被丢掉的都计入 `stats().dropped`。

> `Block` 是默认值，因为「日志丢了却没人知道」比「业务慢一点」更难排查。
> 但对延迟敏感的服务，`Block` 意味着日志写入会把背压传给业务线程 ——
> 高并发场景建议用 `DropDebug`：级别低的先牺牲，`ERROR` / `FATAL` 尽量留住。

`DropDebug` 的判断是「按级别从低到高，从队尾往前找第一条可以牺牲的」，
`TRACE` 全被清完才轮到 `DEBUG`，以此类推。

## 队列指标

```cpp
const LogStats s = Logger::get_instance().stats();
// s.queue_length        快照时刻队列长度
// s.queue_peak          队列峰值（是否见过满队）
// s.max_write_latency_us 从打点到写完的最长延迟
```

`queue_peak` 逼近 `buffer_size` 就说明容量不够，或者写出速度跟不上；
`max_write_latency_us` 突然变大通常意味着磁盘抖动或 Sink 慢。

这两个值的更新是热路径上的原子操作（relaxed 读改写，无锁），不在入队时取锁。

## flush_all()

```cpp
Logger::get_instance().flush_all();
```

- 异步：等队列清空**且**当前这条写完，然后 flush 所有 Sink
- 同步：直接 flush 所有 Sink
- 两种情况都会先给当前线程的聚合去重序列补一条摘要（见下）

只保证**本进程已产生**的日志落盘，不保证跨进程。测延迟分布时在压测中间调用它，
可以把异步队列的积压排空，避免把排队时间算进写盘时间。

## close()

```cpp
const LogStats final_stats = Logger::get_instance().close();
```

- 停止接收新日志、等后台线程把队列消费完、flush 所有 Sink、join 线程
- 可重复调用、可并发调用（内部 `std::call_once`，只有一个线程真正执行停止）
- 返回最终的统计快照
- **单例的析构函数**等价于调用 `close()`，所以正常退出不需要显式调用。
  `with()` 得到的子 Logger 共享同一份状态，它的析构不会关闭日志器

`close()` 是**终态**：调用之后不应再打日志。之后产生的日志会被丢弃并计入
`dropped`（不是静默消失，也不是写进已关闭的 Sink）。

> 单例生命周期与进程同长；测试里如果需要「干净重启」，用独立的测试二进制
> （本项目的 `test_async_close`、`test_lifecycle` 就是这么做的），不要试图原地复用一个
> 已经 `close()` 过的 Logger。

## 异步不可回退

```cpp
cfg.async = false;
Logger::get_instance().set_config(cfg);   // ⚠️ 不会停掉后台线程
```

后台线程一旦启动就常驻：`set_config` 收到 `async = false` 时会把配置里的 `async`
强制改回 `true`。理由——线程无法安全地「停了再启」（正被 join / 正持有锁时停都不安全），
而「线程还在但不再消费队列」比维持异步更糟（日志会静默堆积到内存耗尽）。

要真正停下只有 `close()`。

## 聚合去重与 flush

聚合去重（`dedup_window_ms`）按线程累积计数，`flush_all()` 会给**当前线程**补发未结束的
序列摘要，其它线程的序列由它们各自的下一条不同日志收尾。

```cpp
cfg.dedup_window_ms = 1000;
LOG_INFO("retrying");     // 输出首条
LOG_INFO("retrying");     // 抑制
Logger::get_instance().flush_all();   // 补发 "retrying (repeated 2 times)"
```

短生命周期进程（命令行工具、一次性任务）开着去重又只打几条日志时，
别忘了退出前 `flush_all()`，否则最后的摘要可能没机会发出来。

## 相关

- [性能报告](../performance.md) — 两种模式的实测数据
- [故障排查](troubleshooting.md) — 日志少了 / 卡住了怎么查
- [file-rotation.md](file-rotation.md) — 异步模式下写盘变慢的常见原因
