#pragma once
#include <string>
#include <string_view>

// ===== Trace 上下文（M5：Trace 系统集成）=====
// 只做 W3C traceparent 编解码 + 字段约定，不引入 OpenTelemetry SDK。
// logger 只做字段透传，不实现 span 层级、不上报、不主动注入 HTTP 头。

// W3C traceparent 的四段：version "-" trace_id "-" span_id "-" trace_flags
// version / trace_flags 有默认值，构造后不设也能用；trace_id / span_id 必须由调用方填
struct TraceContext {
  std::string version = "00";      // 2 hex，当前规范仅有 "00"
  std::string trace_id;            // 32 hex，全链路唯一，不得为全 0
  std::string span_id;             // 16 hex，当前 span，不得为全 0
  std::string trace_flags = "01";  // 2 hex，bit0 为 sampled；"01" 表示采样

  // 是否采样（trace_flags bit0）；格式非法按未采样处理
  [[nodiscard]] bool sampled() const;
};

// 生成一条新链路（作为链路起点）：version / trace_flags 取默认值，
// trace_id / span_id 随机且保证非全 0
[[nodiscard]] TraceContext generate_trace();

// 解析 traceparent："00-<32hex>-<16hex>-<2hex>"（更高版本可追加字段，按规范忽略）
// version / 长度 / 字符 / 全 0 任一不符即返回 false（调用方按「无 trace」处理）
bool parse_traceparent(std::string_view header, TraceContext& out);

// 生成供下游传播的 traceparent 头。
// 四个字段任一非法（长度/字符/全 0）即返回空串——不做任何默认值替换，
// 避免发出「看似合法但语义被改过」的头。调用方按「无 trace」处理。
[[nodiscard]] std::string make_traceparent(const TraceContext& ctx);
