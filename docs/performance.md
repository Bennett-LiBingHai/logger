# 性能报告

**一句话结论**：M4 的四项性能目标全部达标；新增的 M5 / M6 能力里，**去重抑制是最省的路径**
（51 ns），**异常提取与堆栈符号化最贵**（各约 3.9 µs / 8.5 µs）。
另有一处已修的热点：宏入口渲染调试信息用了 `ostringstream`，改成按线程缓存后
**文本路径快了约 20%**（`SyncText` 1173 → 912 ns）。

## 测试环境

| | |
|---|---|
| CPU | 16 vCPU（WSL2 on Windows） |
| 内核 | 6.6.87.2-microsoft-standard-WSL2 |
| 编译器 | gcc 13，`-O3 -DNDEBUG`（`CMAKE_BUILD_TYPE=Release`） |
| 基准框架 | Google Benchmark 1.8.3（Ubuntu 包） |
| 取数 | 每场景 5 轮重复，`--benchmark_min_time=0.3s`，取**中位数** |

## 怎么跑

```bash
sudo apt install libbenchmark-dev                     # 可选依赖，没装就跳过基准目标
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target logger_benchmark
./build-release/logger_benchmark

# 只跑一组 / 看跨轮稳定性 / 存 JSON
./build-release/logger_benchmark --benchmark_filter=Sync
./build-release/logger_benchmark --benchmark_repetitions=5 --benchmark_report_aggregates_only
./build-release/logger_benchmark --benchmark_format=json --benchmark_out=bench.json
```

## 口径与限制

先看口径，否则数字会被误读。这四条都是实测确认过的：

**1. 全部基准用 `UseRealTime`。** 不加它时 gbench 的 `items_per_second` 按 **CPU 时间**算，
多线程下会给出误导性的数：同一个基准，4 线程不带是 3.4 M/s，带是 12.8 M/s。
（用一份已知工作量的对照基准反推出来的，见提交记录。）带上之后
`Iterations` 是**所有线程的总和**，`Time` 是聚合墙钟 ns/op —— 和"吞吐"的通常含义一致。

**2. 系统包的 libbenchmark 是 DEBUG 版。** Ubuntu 的 `libbenchmark1.8.3` 打包时没开 `NDEBUG`，
运行时固定打一行 `***WARNING*** Library was built as DEBUG. Timings may be affected.`。
框架自身开销因此比 Release 版略高，**对 ns 级场景影响相对更大**。

**3. 延迟分位有约 20–30 ns 的底线。** P50/P95/P99 在计时循环之后单独采样，每次采样含两次
`steady_clock::now()`，所以 `DisabledLevel` 的 P50 是 30 ns 而它的 ns/op 只有 3 ns ——
**分位数对 ns 级场景没有分辨力**，只在 µs 级及以上有意义。

**4. 不同轮次之间会有波动。** 单轮内 CV 很小（0.01%–0.22%），但跨轮次不同：
`SyncText` 在 1173–1258 ns 之间、并发场景波动可达 10% 量级。下表取自同一轮。

> 与旧版自研 harness 的数字**不可直接对比**：那套是固定迭代 + 单轮 + 无编译屏障，
> 新版是自适应迭代 + 5 轮取中位数 + `DoNotOptimize`。差在 10% 以内的场景（同步文本、
> 并发、文件）可以互相印证，差得多的（异步入队）是口径不同，不是行为变了。

## 结果

### 基础路径

| 场景 | ns/op | 吞吐 | P50 | P95 | P99 | CV |
|---|---:|---:|---:|---:|---:|---:|
| `DisabledLevel`（级别 OFF） | **3** | 306.4 M/s | 21 ns | 32 ns | 32 ns | 0.02% |
| `SyncText`（宏入口 + 文本） | 912 | 1.10 M/s | 892 ns | 944 ns | 1473 ns | 0.03% |
| `SyncJson`（结构化 + JSON） | 1190 | 0.84 M/s | 1172 ns | 1204 ns | 1712 ns | 0.03% |
| `SyncWithFields`（2 个 KV） | 990 | 1.01 M/s | 965 ns | 995 ns | 1141 ns | 0.02% |

### M5 / M6 能力（此前从未测过）

| 场景 | ns/op | 吞吐 | P50 | P95 | P99 | CV |
|---|---:|---:|---:|---:|---:|---:|
| `SyncWithContext`（上下文 2 字段） | 1265 | 0.79 M/s | 1255 ns | 1297 ns | 1628 ns | 0.03% |
| `SyncWithMasking`（3 字段 1 命中） | 1244 | 0.80 M/s | 1183 ns | 1203 ns | 1266 ns | 0.01% |
| `SyncWithDedup`（重复被抑制） | **51** | 19.60 M/s | 73 ns | 83 ns | 83 ns | 0.01% |
| `SyncWithEscape`（正文含转义字符） | 1196 | 0.84 M/s | 1121 ns | 1162 ns | 2043 ns | 0.01% |
| `SyncWithLimits`（触发预算裁剪） | 8405 | 0.12 M/s | 8225 ns | 8516 ns | 16856 ns | 0.02% |
| `SyncException`（嵌套异常提取） | 4828 | 0.21 M/s | 4720 ns | 4875 ns | 7292 ns | 0.01% |
| `StacktraceCapture`（只采集） | 1533 | 0.65 M/s | 1483 ns | 1546 ns | 2344 ns | 0.01% |
| `StacktraceFullPath`（采集 + 符号化） | 9399 | 0.11 M/s | 8961 ns | 9666 ns | 19539 ns | 0.01% |

### 并发、I/O 与异步

| 场景 | ns/op | 吞吐 | P50 | P95 | P99 | CV |
|---|---:|---:|---:|---:|---:|---:|
| `ConcurrentText`（4 线程同步） | 457 | 2.19 M/s | — | — | — | 0.04% |
| `FileSinkWrite`（含文件写入） | 1806 | 0.55 M/s | — | — | — | 0.01% |
| `AsyncEnqueue`（异步入队） | 463 | 2.16 M/s | 290 ns | 1131 ns | 1857 ns | 0.23% |
| `AsyncQueueFullDrop`（队列满丢弃） | 312 | 3.21 M/s | — | — | — | 0.03% |

并发场景不报分位：多线程下逐次计时会互相干扰（4 个线程同时读时钟）。

## 与 M4 目标对照

| 目标 | 实测 | 结论 |
|---|---|---|
| 关闭级别开销尽量低 | 3 ns/op（含框架开销） | ✅ 接近普通函数调用 |
| 同步文本 ≥ 100K 条/秒 | 1.10 M/s | ✅ 11× |
| 异步文本 ≥ 300K 条/秒 | 2.16 M/s | ✅ 7.2× |
| 关键路径额外延迟 P99 < 1ms | 同步文本 P99 1.5 µs | ✅ 三个数量级余量 |

## 结论与发现

### 1. 宏入口的流开销已消除（1173 → 912 ns）

同样内容、同样无字段，只差入口 —— 下面是同一份对照基准在改动前后的实测：

| 入口 | 改前 | 改后 |
|---|---:|---:|
| `LOG_INFO(...)`（宏，带 file/line/func） | 1210 ns | **860 ns** |
| `Logger::get_instance().info(...)`（结构化） | 734 ns | 760 ns |

根因：宏入口要渲染 `[thread_id][file:line][func]`，而它用了一个 `std::ostringstream`
（[text_formatter.cpp](../src/detail/formatter/text_formatter.cpp)）。
`std::thread::id` 只能经 `operator<<` 输出，但**一个线程的 id 不会变** ——
按 id 缓存一次字符串即可（`thread_id_str()`，单槽 `thread_local`；异步下格式化发生在后台
线程、记录里存的是业务线程的 id，所以按 **id 值**命中而不是"当前线程"）。

**顺带验证了另一个方向是错的**：既然是拼字符串，很容易想到"整体用 `ostringstream` 会不会更快"。
实测同一条记录文本（5 轮中位数）：

| 拼法 | ns/条 |
|---|---:|
| 局部 `string` 从头 `+=`（原实现） | 130 |
| **整体用 `ostringstream`** | **279** |
| 局部 `string` + `reserve`（现实现） | 88 |
| `thread_local` 复用 buffer | 72 |
| `thread_local` 复用 + 最后拷贝一次（`SinkInput` 按值持有结果） | 87 |

整体用流**慢一倍多**：流插入每个部件都要过一次 sentry / locale / 虚函数调用，而且
`oss.str()` 在 C++17 里是**拷贝**。`+=` 的开销则主要在扩容。所以正确做法是两件小事：
**去掉流**、**预留长度**（`reserve(96 + content + 48 × fields)`，估小了只是多一次扩容）。
`thread_local` 复用看着最快，但结果要按值交出去、必须拷一次，收益被吃掉，还引入线程局部状态 ——
不值。

净效果（跨轮波动约 2%）：**文本 + 宏入口的场景普遍快 20%**（`SyncText` −22%、
`SyncWithEscape` −22%、`SyncWithContext` −20%），结构化入口与 JSON 基本不变 ——
JSON 里的线程 id 短，流走的是 SSO、本来就没有堆分配。

### 2. 去重抑制路径只有 51 ns —— 比正常路径省 95%

`SyncWithDedup` 51 ns vs `SyncWithFields` 990 ns。设计意图达成：去重判定排在字段合并、
堆栈采集、`Record` 构造**之前**，被抑制的日志这些开销全部不付。
刷屏场景开着 `dedup_window_ms` 几乎不花钱。

### 3. 预算裁剪触发时慢 8.5 倍

`SyncWithLimits` 8405 ns vs `SyncWithFields` 990 ns。原因在
[logger.cpp](../src/logger.cpp) 的 `format_record_budgeted`：
超预算后要**反复重排格式** —— 先排全量，再排"最少形态"，然后每丢一个字段重排一次，
截断消息后再排一次。本例 3 个字段 → 约 6 次完整格式化。

这是降级路径的固有代价（"每丢一个字段重排一次"保证了丢得最少），但在
**大字段 + 小预算**的组合下会被放大。正常配置（`max_record_size = 1024`、常规消息）
不会进入这条路径。

### 4. 异常与堆栈都不便宜，但只该在真出事时付

| | 绝对值 | 相对普通日志的增量 |
|---|---:|---:|
| `SyncException`（嵌套链提取） | 4828 ns | +3.9 µs |
| `StacktraceFullPath`（采集 + 符号化） | 9399 ns | +8.5 µs |
| 其中：只采集（独立测，不含日志路径） | 1533 ns | — |
| 其中：符号化（`dladdr` + demangle） | — | ≈ 7 µs |

- 异常提取贵在 `__cxa_demangle` 与嵌套链的 `rethrow_if_nested` 逐层展开
- 堆栈贵在符号化。**FATAL 默认自动采栈**，所以一条 FATAL 约 9 µs ——
  致命错误通常只发生一次，这个代价可以接受；若要压成本，改
  `config.stacktrace = StackTraceMode::OFF`，或只保留采集（`capture()` 只要 1.5 µs，
  符号化推迟到真要看的时候离线做）

### 5. 并发只有 2.2× 加速 —— 写路径全程持全局锁

4 线程 2.19 M/s vs 单线程 1.10 M/s，只有 **2.0×**（16 vCPU 上）。原因是同步写路径在
`impl_->mtx` 保护下完成格式化与写 Sink，本质是串行的；能并行的是加锁之前的部分
（字段合并、部分编码）。

这不是缺陷（单例 + 多 Sink 的语义要求写序一致），但如果将来需要更高的并发上限，
下手点是**减小临界区**：把格式化移出锁（Sink 写入仍需互斥）。

> 改动前是 2.2×（2.07 / 0.85 M/s）。单线程变快之后，串行段的占比更高，比例反而略降 ——
> 这是"分子分母同时变"的错觉，绝对吞吐是涨的。

### 6. 上下文与脱敏的单条开销

以 `SyncText`（912 ns，宏入口无字段）为基准：

| | 增量 |
|---|---:|
| 上下文（2 个字段，含合并与去重） | +353 ns |
| 脱敏（3 字段，1 个命中；基准是 `SyncWithFields` 990 ns） | +254 ns |

上下文的开销主要在字段合并与去重（每条都要把上下文栈的字段并入）。长期存活的字段
（服务名、版本号）用 `with()` 预绑定比每次开 `ContextScope` 便宜 —— `with()` 的字段随
快照一起拿到，不走每条合并。

## 复现

```bash
# 全量（约 1 分钟）
./build-release/logger_benchmark --benchmark_repetitions=5 --benchmark_report_aggregates_only

# 单场景
./build-release/logger_benchmark --benchmark_filter=BM_Stacktrace

# 结果存 JSON，便于对比两次改动
./build-release/logger_benchmark --benchmark_format=json --benchmark_out=before.json
```

对照两次改动时直接 diff JSON 里的 `real_time` 中位数（Ubuntu 的包没带 `compare.py`，
要它得去 gbench 源码里取）。**注意**：同一台机器上跨轮次波动可达 10%，判断"改动是否有效"
要看差异是否明显超过这个量级。

> 建议固定这些参数再比较：同一台机器、`--benchmark_repetitions=5`、
> `--benchmark_min_time=0.3s`、机器空闲。CI 上跑基准数字噪声太大，不适合卡阈值。

## 相关

- [benchmark/logger_benchmark.cpp](../benchmark/logger_benchmark.cpp) — 基准源码
- [同步与异步指南](guides/sync-and-async.md) — 模式选型
- [milestone.md](milestone.md) — M4 性能目标
