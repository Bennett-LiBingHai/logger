# Logger

一个 C++17 的日志库。当前进度：**M1 最小可用版本（MVP）**。

## 特性（M1）

- 六种日志级别：`TRACE / DEBUG / INFO / WARN / ERROR / FATAL`，外加 `OFF` 关闭全部
- 宏接口 `LOG_TRACE` … `LOG_FATAL`，支持 printf 风格变参格式化
- 控制台输出按级别分流：`Error` 及以上 → stderr，其余 → stdout
- 全局日志级别过滤
- 多线程安全（互斥锁保护，TSan 可验证）
- 基础配置：级别、单条日志最大长度、时间格式、UTC/本地时间
- 超长日志自动截断
- 输出目标抽象（Sink），可扩展自定义 Sink

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

    LOG_INFO("user %d login", 1001);
    LOG_WARN("disk usage %d%%", 92);
    LOG_ERROR("connect failed: %s", "timeout");

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

低于 `config.log_level` 的日志会被忽略（在宏入口处尽早过滤）。

## 配置

| 字段 | 默认值 | 说明 |
|---|---|---|
| `log_level` | `LogLevel::TRACE` | 全局最低输出级别 |
| `max_log_item_size` | `1024` | 单条日志最大长度（超长截断） |
| `time_format` | `TimeFormat::ISO8601` | 时间格式 |
| `use_utc_time` | `false` | 是否使用 UTC（否则本地时间） |

```cpp
LogConfig cfg;
cfg.log_level = LogLevel::INFO;
cfg.use_utc_time = true;
Logger::get_instance().set_config(cfg);
```

## 输出格式

```
2026-09-04T11:50:32.077[INFO][<thread_id>][<file>:<line>]<content>
```

## 测试

```bash
cmake -S . -B build          # BUILD_TESTS 默认 ON
cmake --build build
ctest --test-dir build
```

- `test_logger`：单元 + 集成用例（级别、过滤、格式化、多 Sink、截断、并发等）
- `test_console`：手动观察 stderr 分流的用例

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
├── include/logger/            # 公共头文件
│   ├── logger.h               # Logger 核心 + LOG_* 宏
│   ├── level.h                # 级别枚举
│   ├── config.h               # 配置
│   ├── record.h               # 日志记录
│   ├── format_result.h        # 格式化结果
│   ├── sink.h                 # Sink 抽象
│   ├── sink/console_sink.h    # 控制台 Sink
│   └── formatter/text_formatter.h
├── src/                       # 实现
├── examples/                  # 可运行示例
├── test/                      # 单元 + 集成测试
└── docs/                      # 设计文档（milestone.md）
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

完整路线见 [docs/milestone.md](docs/milestone.md)。当前完成 **M1：最小可用版本 MVP**。
