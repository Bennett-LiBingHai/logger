# 破坏性变更清单

按「现象 → 原因 → 迁移」记录已经发生的破坏性变更，供将来写升级指南时取材。

为什么不是「升级指南」：`0.x` 期间没有对外发布过版本，没有用户需要升级；这些条目是**给自己**
留的账。自 v1.0.0 起重大的变更会同时提供正式升级说明，这份清单保留为完整历史。

**当前状态：v1.0.0（2026-09-14）。自本版本起按语义化版本承诺兼容性。**
承诺范围与废弃流程见[兼容性说明](compatibility.md)。

---

## M7（v1.0.0）

### 所有符号收进 `logger::` 命名空间

- **现象**：`Logger::get_instance()`、`KV(...)`、`LogConfig`、`Field`、`StackTrace`、
  `encode(...)` 等不再直接可用，要写成 `logger::X`，或在文件顶部加 `using namespace logger;`。
  宏 `LOG_*` **不受影响**，照常直接用
- **原因**：v1.0.0 之前所有符号都摊在全局命名空间 —— 19 个类型 + 9 个函数。
  `Logger`、`Field`、`encode` 这类名字与业务代码撞车的概率不低（`Logger` 尤其常见），
  而命名空间是零成本的隔离。宏放不进命名空间，所以 `LOG_*` 仍是全局的（内部已用全限定名）
- **迁移**：在用到库的 .cpp 顶部加 `using namespace logger;`（推荐），或写全限定名
  `logger::Logger::get_instance()`。内部实现从全局移到了 `logger::detail`，
  用到 `detail/` 头的代码本来就不受承诺保护

### 自定义类型的 `encode` 可以定义在自己的命名空间里了

- **现象**：以前 `encode(const Money&, std::string&, bool)` 必须定义在**全局命名空间**，
  放进 `namespace my { }` 会找不到；现在两种位置都行
- **原因**：库内原本是限定调用 `::encode(...)`，限定调用不触发 ADL。收进命名空间时改成了
  非限定调用（配合 `using ::logger::encode;` 把库的重载带进作用域），ADL 就生效了
- **迁移**：不需要。原来是全局的那份继续有效

### `with()` 得到的子 Logger 析构不再关闭日志器

- **现象**：以前 `auto lg = Logger::get_instance().with(KV(...))` 的 `lg` 一离开作用域，
  整个日志器就停止工作（异步下 `flush_all()` 永久挂起，之后的日志静默丢失）。
  现在子 Logger 析构是空操作。
- **原因**：`~Logger()` 无条件调用 `close()`，而子 Logger 与单例**共享同一份状态**。
  一个临时子对象就能把全局日志器关掉。这是缺陷不是设计。
- **迁移**：不需要。若你此前"利用"了这个行为来做关闭，请改成显式调用
  `Logger::get_instance().close()`。

### `close()` 之后继续打日志：从「仍会写入」变为「丢弃并计数」

- **现象**：同步模式下，`close()` 之后打的日志以前照样写进 Sink，现在会被丢弃并计入
  `LogStats::dropped`。
- **原因**：`close()` 的语义是「停止接收新日志」（milestone M4 §4）。旧的实现只停了异步消费方，
  没收停接收方 —— 异步 `Block` 策略下这会让调用方永久阻塞在一个永远不会到来的队列空间上。
- **迁移**：不要在 `close()` 之后打日志。确实需要继续输出就把日志送到另一个还活着的 Sink，
  或者干脆不要调用 `close()`（让析构在进程退出时处理）。

### 内部头文件移入 `detail/`，`FormatResult` 改名 `SinkInput`

- **现象**：`#include "logger/formatter.h"`、`logger/record.h`、`logger/utiils.h` 等路径不再存在；
  自定义 Sink 里的 `log(const FormatResult&)` 编译失败。
- **原因**：公共头与内部头没有边界，用户能调用到随时会改的内部符号。现在内部实现统一放在
  `include/logger/detail/`，Doxyfile 按目录整体排除（比逐个列符号名好维护）。
- **迁移**：
  - `FormatResult` → `SinkInput`（定义在 `logger/sink.h`）
  - 自定义 Sink 改签名为 `bool log(const SinkInput& input) override;`
  - 用到过内部头文件的，改回公共头；`detail/` 下的内容不承诺稳定

### `Logger` 的构造函数私有化，拷贝/移动赋值禁用

- **现象**：`Logger lg;` 编译失败；`a = b;` 编译失败。
- **原因**：`Logger` 是共享状态的手柄，不是可以随意复制/赋值的值类型。默认构造出来的实例
  自持一份空状态，赋值则会让两个对象指向同一份状态却各持一份字段 —— 都是误用。
  现在只能通过 `get_instance()` 拿单例，或通过 `with()` 拿共享状态的副本。
- **迁移**：改用 `auto lg = Logger::get_instance().with(...)`。

### `KV` 的 key 长度改按 `strlen` 取

- **现象**：传非常量数组（如 `char buf[32]`）时，以前字段名会带上数组里剩余位置的 NUL 字节
  （`key.size()` 是数组长度减一）；运行期为空的数组还会绕过 `key.empty()` 检查，
  变成一串 NUL 的字段名写进日志。
- **原因**：字面量重载用 `N - 1` 当长度，这对字符串字面量成立，对运行期数组不成立。
- **迁移**：不需要（旧行为是错的）。字面量下编译器把 `strlen` 折叠成常量，没有运行期开销。

---

## M6（v0.6.0）

### `max_log_item_size` → `max_record_size`，字节硬切改为分层预算

- **现象**：`cfg.max_log_item_size = N` 编译失败。
- **原因**：原来的做法是对**格式化结果**做字节切片，会切掉行尾换行、把 JSON 切成半个对象、
  把 UTF-8 切成半个字符。改为三层预算（`max_message_length` / `max_field_length` /
  `max_record_size`），超限时按「先丢尾部字段 → 再截断消息 → 最少形态」降级，
  截断按 UTF-8 边界。
- **迁移**：`cfg.max_log_item_size = N` → `cfg.max_record_size = N`。
  想单独限制正文或单个字段，再用新增的两个配置项。

### 无嵌套异常时不再输出 `error_chain`

- **现象**：只有一层异常时，日志里不再有 `error_chain` 字段（以前会重复一遍 `error`）。
- **原因**：`error` 与 `error_type` 已经是最内层（根因）；`error_chain` 只在存在 caused-by 链时
  才有额外信息。以前用空 key 占位再跳过，行为一致但字段总是出现。
- **迁移**：依赖 `error_chain` 存在的查询要容错。用 `error` / `error_type` 做检索，
  把 `error_chain` 当可选的补充。

### FATAL 默认自动采集调用栈

- **现象**：`LOG_FATAL` 输出里多了 `stacktrace` 字段，且采集有开销。
- **原因**：致命错误通常只发生一次，值得付采集开销（采集只取地址，符号化在格式化时才做）。
- **迁移**：不想要就 `cfg.stacktrace = StackTraceMode::OFF`。

### 比例采样改为聚合去重

- **现象**：M5 设计文档里写的采样配置项不存在，取而代之的是 `dedup_window_ms`。
- **原因**：采样会连"首次发生"一起丢掉 —— 一个只在凌晨出现一次的致命错误可能被采掉，
  而刷屏的噪音反而留下。聚合去重的语义是「相同来源的重复折叠成首条 + 次数」，信息量更大。
- **迁移**：采样只在设计阶段存在过，从未实现，没有代码需要改。

---

## M4（v0.4.0）

### 移除 `AsyQueFulStrategy::SyncFallback`

- **现象**：队列满时"改为同步写入"这个策略值不存在了。
- **原因**：业务线程的延迟特性会随队列状态突然变化（平时是入队，拥堵时变成写盘），
  排队时间不可预期。要「不丢」就用 `Block`，要「不阻塞」就用三个 Drop 策略，边界清晰。
- **迁移**：改用 `Block`（要尽量不丢）或 `DropNewest` / `DropOldest` / `DropDebug`（要绝不阻塞）。
  该策略在同一天的开发过程中就被移除，未出现在任何发布版本里。

---

## M2（v0.2.0）

### printf 风格 → `{}` 位置参数

- **现象**：`LOG_INFO("user %d login", id)` 输出的字面量就是 `%d`，参数没有插进去。
- **原因**：M1 的实现是 `snprintf(buf, sizeof(buf), fmt, args...)`。`%d` 与实参类型不匹配
  （传了 `long` 却写 `%d`）是未定义行为，编译器只能对字面量串给出警告，检查不全。
  改为自研格式化器：按实参类型编码，类型安全，也不依赖 iostream。
- **迁移**：`LOG_INFO("user %d login", id)` → `LOG_INFO("user {} login", id)`；
  `%s` / `%f` / `%ld` 一律写成 `{}`。

---

## 待改进项

已知问题，尚未决定怎么处理。记在这里免得忘掉。

| 项 | 说明 |
|---|---|
| 用户字段与 JSON 保留名重名不被拦截 | `KV("level", "boom")` 会产生重复键（`"level"` 出现两次），解析器行为不一致（Python 取最后一个，有的库直接报错）。保留名见[兼容性说明](compatibility.md#6-json-字段兼容性) |
| 消息正文不参与脱敏 | `LOG_INFO("password={}", pwd)` 不会被拦截 —— 正文没有 key 可判断。只能在文档里强调 |
| `char buf[N]` 走字面量重载 | 现在长度取对了，但它仍是字面量重载（`N > 1` 的 `static_assert` 对运行期数组没意义）。彻底区分需要在接口上约束真字面量 |
