# 上下文指南

> 下文示例假定文件顶部有 `#include <logger/logger.h>`（统一入口，其余公共头都由它带进来）
> 和 `using namespace logger;`（库的公共符号都在 `logger::` 下，只有 `LOG_*` 宏在全局）。
> 示例里出现的 `#include <logger/xxx.h>` 只是为了指明那个能力定义在哪个头，
> 实际只需包含统一入口 `logger/logger.h`。


一条请求会流经很多函数，而 `request_id` 这类字段每个函数都要带。把它们沿着调用链
一路传参既啰嗦又容易漏。上下文解决的就是这个问题：**进入作用域时声明一次，作用域内
所有日志自动带上**。

## ContextScope

```cpp
#include <logger/context.h>

void handle(const Request& req) {
    ContextScope ctx{KV("request_id", req.id), KV("user_id", req.user)};

    LOG_INFO("start handling");     // 自动带 request_id / user_id
    do_work();                      // 里面所有 LOG_* 也带
    LOG_INFO("done");
}   // 出作用域，字段自动出栈 —— 不用手动清理，异常路径也一样
```

- 构造入栈、析构出栈（RAII）。抛异常提前退出函数，字段照样会被清掉
- 栈是 `thread_local` 的 `std::vector`，嵌套时是**内层覆盖外层**同名 key
- 同一线程里嵌套多少层都行，严格 LIFO
- 不能拷贝 / 移动（拷贝会破坏 RAII 语义：两份对象只能析构一次，栈就不平衡了）

## 字段优先级

合并顺序是：`with()` 预绑定 → `ContextScope` → 调用点 `KV`，随后同 key 后写覆盖。
所以：

> **调用点 `KV` > `ContextScope` > `with()` 预绑定**

```cpp
auto lg = Logger::get_instance().with(KV("env", "prod"));

void f() {
    ContextScope ctx{KV("env", "staging")};
    lg.info("a");                              // env=staging（上下文压过 with）
    lg.info("b", KV("env", "dev"));            // env=dev（调用点压过上下文）
}
```

这样设计的理由：越靠近调用点，对「这次日志该带什么」知道得越清楚。

## 跨线程

`ContextScope` 是 `thread_local`，**不跨线程**：线程池里跑的子任务拿不到提交方的上下文。

```cpp
ContextScope ctx{KV("request_id", req.id)};
pool.submit([&] {
    LOG_INFO("in worker");      // ✗ 没有 request_id —— 另一个线程
});
```

想延续就显式传值。`with()` 返回一个共享 sinks / config 的子 `Logger`，可以随意拷贝给子任务：

```cpp
auto sub = Logger::get_instance().with(KV("request_id", req.id));
pool.submit([sub] {
    sub.info("in worker");      // ✓ 带 request_id
});
```

也可以在新线程里重新建一个 `ContextScope`，字段从参数来 —— 哪种都可以，库不替你选。

> 为什么不做跨线程传播：自动传播要么依赖线程池的 hook（`std::thread` 没有），
> 要么在每次提交时捕获一份上下文并绑定生命周期，两者都会把库耦合进调用方的
> 并发模型。显式传一个可拷贝的 `Logger` 更简单，代价也看得见。

## with() 的便捷方法

常用字段有现成的：

```cpp
auto lg = Logger::get_instance();
lg.with_request_id("req-42").info("...");     // KV("request_id", ...)
lg.with_trace_id("4bf9...").info("...");      // KV("trace_id", ...)
lg.with_span_id("00f0...").info("...");       // KV("span_id", ...)
lg.with_user_id("u-7").info("...");           // KV("user_id", ...)
lg.with_service("order-api").info("...");     // KV("service", ...)
lg.with_trace(tc).info("...");                // trace_id + span_id + trace_flags
```

`with_trace(TraceContext)` 一次带齐三个链路字段，见 [Trace 集成指南](trace.md)。

## 与其它能力的关系

| 能力 | 作用范围 |
|---|---|
| 脱敏 | 上下文字段**也参与**脱敏（先合并、后脱敏） |
| 聚合去重 | 判定早于上下文合并 —— 重复日志连字段都不用合并，省掉这部分开销 |
| 长度预算 | 上下文字段与调用点字段同等对待，超长按同样规则截断 |
| 异步 | 字段在入队前就已物化进 `Record`（值拷贝），后台线程不会读到已析构的上下文 |

顺序上有一点值得注意：**脱敏在合并之后**，所以上下文里带了 `token` 之类的字段同样会被
屏蔽；但如果调用点用同名字段覆盖了它，屏蔽的是覆盖后的值。

## 什么时候不该用

- 字段只在一条日志里出现 → 直接写在 `KV` 里，别为它开作用域
- 字段是「进程级」常量（服务名、版本号）→ 用 `with()` 建一个长期存活的 `Logger`，
  每个请求建一次作用域是白花开销
- 需要跨线程 / 跨异步边界带着走 → 用 `with()` 显式传，或者干脆在目标线程重建

## 相关

- [Trace 集成指南](trace.md) — 链路字段的接续与传播
- [结构化日志指南](structured-logging.md) — 字段顺序与覆盖规则
