# Logger

一个 C++17 的日志库。当前进度：**M4 异步、并发与高性能**。

## 特性

- 七种日志级别：`TRACE / DEBUG / INFO / WARN / ERROR / FATAL`，外加 `OFF` 关闭全部
- 宏接口 `LOG_TRACE` … `LOG_FATAL`，支持 `{}` 位置参数格式化
- 结构化字段 `KV("key", value)`，文本输出 `key=value`，JSON 输出带类型成员
- 文本 / JSON 两种输出格式，`config.format` 切换
- 预绑定字段 `with(KV(...))`，共享 sinks/config，返回子 Logger
- 字段去重（同 key 后写覆盖）、空 key 跳过
- 控制台输出按级别分流：`Error` 及以上 → stderr，其余 → stdout
- 全局日志级别过滤
- 多线程安全（互斥锁保护，TSan 可验证）
- 超长日志自动截断
- 文件输出 + 按大小/时间自动轮转，可限制保留文件数
- 文件异常自愈（目录缺失自动创建、文件被外部删除自动重开）
- 写失败策略（降级 stderr / 丢弃并计数），失败不崩溃
- 异步写入（可选开关，惰性启动后台线程，业务线程入队即返回）
- 队列满策略（Block / DropNewest / DropOldest / DropDebug / SyncFallback）
- 优雅关闭（`close()` / 析构自动等待队列清空并刷盘）

## 依赖

- CMake ≥ 3.16
- 支持 C++17 的编译器（gcc / clang）
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

完整可运行示例见 [examples/basic_usage.cpp](examples/basic_usage.cpp)，直接跑：

```bash
cmake --build build --target logger_example
./build/logger_example
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

低于 `config.log_level` 的日志会被忽略（在入口处尽早过滤）。

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

字段规则：

- 按调用顺序输出；同 key 后写覆盖（last-write-wins）
- 空 key：字面量 key 编译期 `static_assert` 拦截；运行时动态 key 直接跳过
- 支持类型：字符串、各宽度整数、浮点（含 nan/inf）、布尔、`char`、`std::vector`、时间点、带 `operator<<` 的自定义类型

## 配置

| 字段 | 默认值 | 说明 |
|---|---|---|
| `log_level` | `LogLevel::TRACE` | 全局最低输出级别 |
| `max_log_item_size` | `1024` | 单条日志最大长度（超长截断） |
| `time_format` | `TimeFormat::ISO8601` | 时间格式 |
| `use_utc_time` | `false` | 是否使用 UTC（否则本地时间） |
| `format` | `LogFormat::TEXT` | 输出格式：`TEXT` / `JSON` |
| `log_fail_strategy` | `LogFailStrategy::FallbackToStderr` | 写失败策略：`FallbackToStderr` / `Drop` |
| `async` | `false` | 异步写入开关（`set_config` 时惰性启动后台线程） |
| `buffer_size` | `10000` | 异步队列最大长度 |
| `asy_que_ful_strategy` | `AsyQueFulStrategy::Block` | 队列满策略：`Block` / `DropNewest` / `DropOldest` / `DropDebug` / `SyncFallback` |

```cpp
LogConfig cfg;
cfg.log_level = LogLevel::INFO;
cfg.format = LogFormat::JSON;
Logger::get_instance().set_config(cfg);
```

## 输出格式

文本（默认）：

```
2026-09-04T11:50:32.077[INFO][<thread_id>][<file>:<line>]<content> key=value ...
```

JSON（`cfg.format = LogFormat::JSON`）：

```json
{"time": "2026-09-04T11:50:32.077", "level": "info", "msg": "order created", "order_id": "ORD-1001", "amount": 99.5}
```

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
- 写失败（磁盘满/权限/句柄失效）不会崩溃，由 `log_fail_strategy` 兜底，并累计 `Logger::stats()`

## 异步写入

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

> 异步一旦开启即不可回退（后台线程常驻）；错误/致命级别日志如需「不丢」保证，可把
> `asy_que_ful_strategy` 设为 `Block` 或 `SyncFallback`。

队列满时的行为由 `asy_que_ful_strategy` 决定：

| 策略 | 队列满时行为 |
|---|---|
| `Block` | 阻塞调用方，尽量不丢日志 |
| `DropNewest` | 丢弃新日志 |
| `DropOldest` | 丢弃最旧日志 |
| `DropDebug` | 优先丢弃低级别日志，保留新日志 |
| `SyncFallback` | 改为同步直写（绕过队列） |

优雅关闭：`close()`（或析构）会停止接收新日志、等待队列消费完并刷新 Sink，返回统计信息。

## 测试

```bash
cmake -S . -B build          # BUILD_TESTS 默认 ON
cmake --build build
ctest --test-dir build
```

- `test_logger`：单元 + 集成用例（级别、过滤、`{}` 格式化、结构化字段、JSON、去重、文件轮转、失败策略、并发等）
- `test_console`：手动观察 stderr 分流的用例
- `test_async`：异步模式（并发无损、五种队列满策略）
- `test_async_close`：异步优雅关闭（`close()` 刷出剩余日志）

### 性能基准

零外部依赖的自研 benchmark，输出吞吐（条/秒）与 P50/P95/P99 延迟：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   # 建议 Release 下测性能
cmake --build build --target logger_benchmark
./build/logger_benchmark
```

覆盖场景：关闭级别、同步文本、同步 JSON、带字段文本、并发、异步文本、文件 Sink。

当前实测结果（开发机 WSL2 / gcc 13 / `-O2`，仅作量级参考）：

| 场景 | 吞吐 | 延迟均值 | P99 |
|---|---:|---:|---:|
| 关闭级别（级别 `OFF`） | 188M/s | 5 ns | 33 ns |
| 同步文本 | 923K/s | 1084 ns | 1.9 µs |
| 同步 JSON | 772K/s | 1295 ns | 1.9 µs |
| 带字段文本 | 1.06M/s | 939 ns | 1.7 µs |
| 并发文本（4 线程聚合） | 1.98M/s | 504 ns | — |
| 异步文本（入队开销） | 1.34M/s | 748 ns | 9.7 µs |
| 文件 Sink | 510K/s | 1962 ns | 10.9 µs |

> 并发为聚合吞吐；异步文本测业务线程入队开销（后台线程并发写 Sink）。

对照 M4 目标：关闭级别开销接近普通函数调用、同步文本 ≥ 100K/s、异步文本 ≥ 300K/s、
关键路径 P99 < 1ms——均达标。详细目标见 [docs/milestone.md](docs/milestone.md) 的 M4 章节。

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
├── include/logger/                 # 公共头文件
│   ├── logger.h                    # Logger 核心 + LOG_* 宏 + with()
│   ├── level.h                     # 级别枚举
│   ├── config.h                    # 配置（含 async / 队列满策略）
│   ├── record.h                    # 日志记录（含结构化字段）
│   ├── field.h                     # KV 字段 + encode 编码入口
│   ├── format.h                    # {} 位置参数格式化
│   ├── formatter.h                 # 格式化结果 FormatResult
│   ├── formatter/text_formatter.h
│   ├── formatter/json_formatter.h
│   ├── literals.h                  # 容量字面量（1_mb 等）
│   ├── utiils.h                    # 时间 / 通用小工具
│   ├── sink.h                      # Sink 抽象
│   ├── sink/console_sink.h         # 控制台 Sink
│   └── sink/file_sink.h            # 文件 Sink（轮转）
├── src/                            # 实现（镜像 include 结构）
│   ├── logger.cpp
│   ├── field.cpp
│   ├── utils.cpp
│   ├── formatter/
│   └── sink/
├── examples/                       # 可运行示例
├── benchmark/                      # 性能基准（零外部依赖）
├── test/                           # 单元 + 集成测试
│   ├── test_helpers.h              # 测试用 Sink 等辅助
│   ├── unit/
│   └── integration/
└── docs/                           # 设计文档（milestone.md）
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

完整路线见 [docs/milestone.md](docs/milestone.md)。当前完成 **M4：异步、并发与高性能**。
