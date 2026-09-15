# 文件轮转指南

> 下文示例假定文件顶部有 `#include <logger/logger.h>`（统一入口，其余公共头都由它带进来）
> 和 `using namespace logger;`（库的公共符号都在 `logger::` 下，只有 `LOG_*` 宏在全局）。
> 示例里出现的 `#include <logger/xxx.h>` 只是为了指明那个能力定义在哪个头，
> 实际只需包含统一入口 `logger/logger.h`。


```cpp
#include <logger/sink/file_sink.h>

FileSinkConfig fc;
fc.dir = "/var/log/app";                 // 日志目录
fc.date_interval_h = 24;                 // 按时间切分间隔（小时），0 = 只按大小
fc.max_file_size = 100 * 1024 * 1024;    // 单文件最大字节，0 = 只按时间
fc.max_backups = 10;                     // 最多保留文件数，0 = 不删除
Logger::get_instance().add_sink(std::make_shared<FileSink>(fc));
```

| 字段 | 默认 | 说明 |
|---|---|---|
| `dir` | 空 | 日志目录；空则 Sink 不可用，写入一律失败 |
| `date_interval_h` | `24` | 按时间切分的间隔（小时），`0` = 不按时间切 |
| `max_file_size` | `10 MiB` | 单文件最大字节，`0` = 不按大小切 |
| `max_backups` | `100` | 最多保留的文件数，`0` = 不删除 |

两个切分条件是**或**的关系，都配就都可能触发；都设为 0 则永不轮转，文件一直追加。
用 [literals.h](../../include/logger/literals.h) 的字节字面量更好读：`fc.max_file_size = 100_mb;`

## 文件名

基准名由轮转时刻的日期决定，格式 `YYYY_MM_DD_HH.log`：

```text
2026_09_13_14.log        ← 基准文件（14 点这次轮转开始写的）
2026_09_13_14.log.1      ← 大小切分产生的第 1 个后继
2026_09_13_14.log.2
2026_09_13_20.log        ← 时间切分后换了基准名，编号从 0 重来
```

`date_interval_h = 24` 时基准名是当天第一次轮转的小时。目录由**进程**创建：
构造 `FileSink` 时若 `dir` 不存在会尝试 `create_directories`，失败不抛异常，
只是后续写入全部失败（由 `log_fail_strategy` 兜底）。

## 触发时机

轮转检查在**每次写入前**做（`FileSink::log` 开头），不是定时器：

| 条件 | 动作 |
|---|---|
| 文件句柄无效，或文件被外部删除 / 改名 | 重新打开当前文件 |
| `now - 文件打开时间 ≥ date_interval_h` | 按时间轮转（换基准名，编号归 0） |
| 已写入字节数 `≥ max_file_size` | 按大小轮转（基准名不变，编号 +1） |
| `max_backups != 0` | 清理超出保留数的文件 |

大小判定用的是**逻辑写入字节数**（`written_` 累加），不是 `file_size()` ——
`ofstream` 有缓冲，`file_size()` 会滞后于实际写入，用它判定会轮转不及时。

## 保留策略

`max_backups` 限制的是**日志目录里的普通文件总数**（含基准文件），超出就按
最后修改时间从旧到新删除。

> ⚠️ 它删的是目录里**所有**普通文件，不只看 `.log` 结尾的。别把其它东西放进日志目录 ——
> 尤其是和日志同目录的 pid 文件、锁文件。

`max_backups = 0` 表示不删除，目录会一直涨。

## 异常自愈

三种常见故障都不会让进程崩：

| 故障 | 表现 | 恢复 |
|---|---|---|
| 目录不存在 | 构造时创建；创建失败则写入失败 | 手动建目录后，下次检查到句柄无效会重开 |
| 文件被外部删除 / 被 `logrotate` 改名 | 句柄还开着但文件已不在 | 下次写入前检测到，重开新文件 |
| 磁盘满 / 权限不足 | `ofs_.fail()` → 返回 false | 由 `log_fail_strategy` 兜底 |

失败**不会**自动重试，也不会自旋等待；每次写入都重新试一次。

## 与 `logrotate` 共存

系统里的 `logrotate` 也能管这个目录，但要注意它默认会**重命名 + 通知进程重开**，
而本库是「检测到文件不在了就重开」，两个机制配合时用 `copytruncate` 更省事：

```conf
/var/log/app/*.log {
    daily
    rotate 7
    copytruncate      # 原地清空，进程不用重开
    missingok
    notifempty
}
```

如果一定要用默认的重命名方式，务必关掉本库自己的轮转（`date_interval_h = 0`、
`max_file_size = 0`），避免两套策略互相打架；文件被改名后本库会自动重开新文件，
这部分是能工作的。

## 多进程

不保证 fork 安全，也**不做跨进程的轮转协调** —— 约定单进程单文件。
多进程部署（比如 prefork 模型）时用 pid 或主机名区分路径：

```cpp
fc.dir = "/var/log/app";
fc.dir /= std::to_string(::getpid());     // 每个进程一个目录
```

## 写失败策略

`Sink::log()` 返回 false 时由 `config.log_fail_strategy` 决定：

| 策略 | 行为 | 计数 |
|---|---|---|
| `FallbackToStderr`（默认） | 降级写 stderr，日志不会因 Sink 故障而消失 | `failed_writes` |
| `Drop` | 直接丢弃 | `failed_writes` + `dropped` |

两种都不抛异常、不重试、不影响业务。失败**不会**进队列重试，也不会自旋等磁盘恢复 ——
每次写入都重新试一次。想感知写失败就定期看 `Logger::stats()`。

判定是**按 Sink** 的：挂了 3 个 Sink 里的 1 个，`failed_writes` 加 1，另外 2 个照常收到日志；
`written` 也照加（至少一个成功即算写入）。

## 相关

- [file_sink.h](../../include/logger/sink/file_sink.h) — 接口注释
- [examples/file_logging.cpp](../../examples/file_logging.cpp) — 可运行示例
- [故障排查](troubleshooting.md) — 日志文件不见了怎么办
