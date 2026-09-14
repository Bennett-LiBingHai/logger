# Trace 集成指南

本库**不实现链路追踪**：不建 span 树、不上报、不主动改 HTTP 头。它只做一件事——
把 W3C traceparent 里的标识接过来，写进日志字段，并把 traceparent 交给下游。

这个边界的理由：追踪后端（Jaeger / Tempo / OTEL Collector）各有各的 SDK，一旦库去依赖
其中任何一个，用另一个的团队就得改库。只传字段则谁都能接——接入成本是几个 `KV`。

## 最小接入

```cpp
#include <logger/trace.h>

TraceContext tc;
if (!parse_traceparent(req.header("traceparent"), tc))
    tc = generate_trace();                        // 没有上游就自己起一条

{
    ContextScope ctx{KV("trace_id", tc.trace_id), KV("span_id", tc.span_id)};
    LOG_INFO("handling request");                 // 自动带链路字段
    process();
}

http_client.set_header("traceparent", make_traceparent(tc));   // 传给下游
```

或者用现成的封装，一行带齐三个字段（`trace_id` / `span_id` / `trace_flags`）：

```cpp
auto lg = Logger::get_instance().with_trace(tc);
lg.info("handling request");
```

完整可运行例子见 [examples/trace_logging.cpp](../../examples/trace_logging.cpp)。

## 三个 API

### `parse_traceparent(header, out) -> bool`

解析 `00-<32hex>-<16hex>-<2hex>` 格式。失败返回 `false`，**`out` 不被写入**。

校验项（任一不符即失败）：

| 项 | 规则 |
|---|---|
| 段数 | 至少 4 段 |
| version | 2 位 hex；为 `"00"` 时**不允许**有追加字段；更高版本按规范忽略追加字段 |
| trace_id | 32 位 hex，不得为全 0 |
| span_id | 16 位 hex，不得为全 0 |
| trace_flags | 2 位 hex |

大小写 hex 都接受。**解析失败应按「无 trace」处理**（不输出链路字段），而不是报错或补默认值 ——
上游头可能是别的系统写的，格式不合规范时静默降级比让请求失败合理。

### `generate_trace() -> TraceContext`

起一条新链路。`version` 取 `"00"`、`trace_flags` 取 `"01"`（采样），
`trace_id` / `span_id` 随机且保证非全 0（W3C 要求）。

熵源是每线程一个 `std::mt19937_64`，`random_device` 不可用时退化为时钟熵 ——
关联 id 不需要密码学强度，不值得为此阻塞或抛异常。

### `make_traceparent(ctx) -> std::string`

生成传给下游的头。**四个字段任一非法就返回空串**，不做任何默认值替换。

> 为什么不像解析那样「静默降级」：解析是接收别人的数据，宽容；生成是发出自己的数据，
> 发一个看起来合法但语义被改过的头（比如把非法 trace_id 换成随机值）会让下游把它当成
> 另一条链路，问题更难查。返回空串调用方立刻知道「这个头我没发出去」。

`trace_flags` 允许 `"00"`（未采样），这不算非法。

### `TraceContext::sampled()`

是否采样，取 `trace_flags` 的 **bit0**，不是「整个字节非 0」：

```cpp
TraceContext tc;
tc.trace_flags = "08";       // bit3 置位，但 bit0 是 0
tc.sampled();                // → false
```

`trace_flags` 是 8 位标志位，只有 bit0 是 `sampled`，其余位是给未来定义的。
格式非法（长度 / 字符不对）时按未采样处理。

## 字段约定

日志里用这三个名字，别自创变体（下游解析、Grafana 的 trace 跳转都按这个认）：

| 字段 | 长度 | 含义 |
|---|---|---|
| `trace_id` | 32 hex | 全链路唯一，跨服务不变 |
| `span_id` | 16 hex | 当前 span |
| `trace_flags` | 2 hex | bit0 = sampled |

`version` **不写进日志** —— 它只参与 traceparent 编解码，对检索没有意义。

## 典型接法

### HTTP 服务端

```cpp
void handle(const Request& req) {
    TraceContext tc;
    if (!parse_traceparent(req.header("traceparent"), tc))
        tc = generate_trace();

    ContextScope ctx{KV("trace_id", tc.trace_id), KV("span_id", tc.span_id)};
    LOG_INFO("{} {}", req.method(), req.path());   // 字段自动带上

    // …处理…

    resp.set_header("traceparent", make_traceparent(tc));
}
```

### 调下游

跨服务调用时**换一个新的 span_id**，`trace_id` 保持不变 —— 这是 span 的语义：
同一条链路，新的一跳。

```cpp
TraceContext next = tc;
next.span_id = generate_trace().span_id;       // 取个新的 span_id
http.set_header("traceparent", make_traceparent(next));
{
    ContextScope ctx{KV("trace_id", next.trace_id), KV("span_id", next.span_id)};
    LOG_INFO("calling payment service");
}
```

### 跨线程

`ContextScope` 不跨线程（见 [上下文指南](context.md)），把 `Logger` 传过去：

```cpp
auto sub = Logger::get_instance().with_trace(tc);
pool.submit([sub] { sub.info("in worker"); });
```

### 后台任务 / 定时任务

没有上游，自己起一条：

```cpp
void cron_job() {
    const auto tc = generate_trace();          // 每次执行一条独立链路
    auto lg = Logger::get_instance().with_trace(tc);
    lg.info("cleanup started");
}
```

## 与追踪后端对接

日志里已经有 `trace_id`，剩下的就是把日志和追踪数据对上。常见做法：

1. **JSON 格式输出**（`cfg.format = LogFormat::JSON`），`trace_id` 是独立成员，采集器直接提取
2. 采集侧（Filebeat / Fluent Bit / OTel Collector）把 `trace_id` 映射到后端字段
3. Grafana / Jaeger 里用 `trace_id` 从日志跳到链路详情

本库不做这一步：日志怎么收集、字段怎么映射，取决于你的日志管道。

## 相关

- [上下文指南](context.md) — `ContextScope` 与 `with()` 的取舍
- [trace.h](../../include/logger/trace.h) — 接口注释
- [W3C Trace Context 规范](https://www.w3.org/TR/trace-context/)
