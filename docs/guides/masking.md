# 脱敏指南

> 下文示例假定文件顶部有 `#include <logger/logger.h>`（统一入口，其余公共头都由它带进来）
> 和 `using namespace logger;`（库的公共符号都在 `logger::` 下，只有 `LOG_*` 宏在全局）。
> 示例里出现的 `#include <logger/xxx.h>` 只是为了指明那个能力定义在哪个头，
> 实际只需包含统一入口 `logger/logger.h`。


日志里最容易泄漏敏感信息的地方是**结构化字段**：`KV("password", pwd)` 这种写法直观、
常见，而且写的人往往没意识到它会落盘。脱敏就是给这类字段加一道统一的闸。

```cpp
LogConfig cfg;
cfg.enable_sensitive_field_mask = true;     // 默认 false，需要显式打开
Logger::get_instance().set_config(cfg);

LOG_INFO("login", KV("user", "alice"), KV("password", "hunter2"));
```

```text
... login user=alice password=***
```

## 默认关闭

`enable_sensitive_field_mask` 默认是 `false`。这是有意的：

- 脱敏会改变数据。开启之前写好的查询、告警、看板如果依赖某些字段的值，打开后会失效 ——
  这种影响应该由使用者主动决定，而不是升级库之后被动接受
- 默认规则只覆盖常见英文关键词，对业务自定义字段名（比如 `card_no`）不一定合适，
  盲目打开会给出「已经安全了」的错觉

生产环境建议显式打开，并把默认关键词表当成起点而非终点。

## 内置规则

命中即把**整个值**替换为 `***`，不看值的形态：

```
password       passwd         token          access_token    refresh_token
secret         authorization  cookie         private_key     credit_card
id_card
```

匹配方式是**精确相等**（`is_sensitive_key` 里就是 `key == k`），不是子串包含。所以：

| key | 是否命中 |
|---|---|
| `password` | ✅ |
| `Password` | ❌ 大小写不同 |
| `user_password` | ❌ 不是精确相等 |
| `pwd` | ❌ 不在表里 |

精确匹配是保守的选择：子串匹配看起来「覆盖更全」，但会把 `token_count`、`secretary_name`
这类正常字段一起打掉，误伤范围不可预测。

`is_sensitive_key()` 是公开的（在 `config.h` 里），自定义 masker 可以复用它，
不用把关键词表抄一遍。

## 自定义 masker

`masker` 是 `std::function<std::string(const std::string& key, const std::string& value)>`，
返回新值；返回值和原值相同时**不计入** `masked_fields`。

```cpp
cfg.sensitive_field_masker = [](const std::string& key, const std::string& value) {
    if (!is_sensitive_key(key))
        return value;                        // 不命中：原样返回，零开销
    if (value.size() <= 10)
        return std::string("***");           // 太短，整体隐藏
    return value.substr(0, 6) + "****" + value.substr(value.size() - 4);  // 卡号留前 6 后 4
};
```

```text
pay credit_card=622202****7890
```

想扩关键词表就自己写一个：`is_sensitive_key(key) || key == "card_no"`。
想按值的内容决定（比如只屏蔽看起来像身份证号的）也可以 —— 拿到的是原始值。

## 入参是原始值

masker 拿到的是**未编码的值**：没有引号、没有转义、数字还是数字。

```cpp
LOG_INFO("t", KV("password", 123456));   // masker 收到的 value 是 "123456"
```

好处是同一个 masker 在文本和 JSON 下行为一致，不用分别处理两种编码。
代价是**被改过的字段一律变成字符串** —— 原始值如果是数字，输出会变成带引号的
`"***"` 而不是 `null` 或数字。字段类型在脱敏后让位于安全，这是自觉的取舍。

没被改动的字段**不会**被重建，类型原样保留：`KV("user_id", 2001)` 在 JSON 下仍是数字 `2001`。
判断依据是 `masker(key, raw) != raw`，所以「原样返回」是零开销的，也不必担心类型退化。

## masker 抛异常会丢整条日志

```cpp
cfg.sensitive_field_masker = [](const std::string& key, const std::string& v) {
    return risky_parse(v);      // 万一抛了
};
```

masker 抛出的异常会向上冒泡，由日志入口统一接住并**丢弃整条记录**（计入 `dropped`）。

> 为什么不是「保留原值继续输出」：那样等于把敏感数据原样写进日志。在「丢一条日志」和
> 「泄漏一个密码」之间，只能选前者。所以自定义 masker 里别做可能抛异常的事，
> 该 `try/catch` 的在 masker 内部处理掉。

## 生效范围

- 对**所有**合并后的字段生效：`with()` 预绑定、`ContextScope`、调用点 `KV`、
  自动附加的 `stacktrace` 都在内
- 在**入队之前**执行，异步模式下队列里不会存明文
- 只作用于**结构化字段**。消息正文（`LOG_INFO("password is {}", pwd)`）不参与脱敏 ——
  正文是自由文本，没有 key 可以判断。**不要往正文里塞敏感信息**
- 同名字段被调用点覆盖时，屏蔽的是覆盖后的值（合并 → 去重 → 脱敏）

## 统计

```cpp
const LogStats s = Logger::get_instance().stats();
// s.masked_fields —— 累计被脱敏的字段数
```

只统计**值真的变了**的字段。用这个数可以粗判规则是否生效（长期为 0 说明关键词表
和实际字段名对不上），也可以用来做「脱敏覆盖率」的巡检。

## 相关

- [config.h](../../include/logger/config.h) — `is_sensitive_key` 与 `default_sensitive_field_masker`
- [examples/masking.cpp](../../examples/masking.cpp) — 可运行示例
- [结构化日志指南](structured-logging.md) — 字段命名建议
