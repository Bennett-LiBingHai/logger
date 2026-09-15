#pragma once
#include <string>
#include <string_view>

namespace logger {
/// @file trace.h
/// @brief W3C traceparent 编解码与链路字段约定。
///
/// 只做字段透传：不实现 span 层级、不上报、也不主动注入 HTTP 头。因此不依赖任何
/// 追踪后端（OTEL / Jaeger / Tempo 都能接），接入成本只是按约定填几个字段。
///
/// @par 典型用法
/// @code
/// TraceContext tc;
/// if (!parse_traceparent(req.header("traceparent"), tc))
///   tc = generate_trace();                       // 没有上游就自己起一条
/// ContextScope ctx{KV("trace_id", tc.trace_id), KV("span_id", tc.span_id)};
/// LOG_INFO("handling request");                   // 自动带上链路字段
/// http_client.set_header("traceparent", make_traceparent(tc));  // 传给下游
/// @endcode

/// @brief 链路上下文：W3C traceparent 的四段。
///
/// version / trace_flags 有默认值，构造后不设也能用；trace_id / span_id 必须由调用方填。
/// 日志字段只用到后三者，version 仅参与 traceparent 编解码。
struct TraceContext {
  std::string version = "00";      ///< 2 位 hex，当前规范仅有 "00"
  std::string trace_id;            ///< 32 位 hex，全链路唯一，不得为全 0
  std::string span_id;             ///< 16 位 hex，当前 span，不得为全 0
  std::string trace_flags = "01";  ///< 2 位 hex，bit0 为 sampled；"01" 表示采样

  /// @brief 是否采样（取 trace_flags 的 bit0，不是"整个字节非 0"）。
  /// @return true 表示采样；trace_flags 格式非法时按未采样处理。
  [[nodiscard]] bool sampled() const;
};

/// @brief 生成一条新链路，作为链路起点。
/// @return version / trace_flags 取默认值，trace_id / span_id 随机且保证非全 0。
[[nodiscard]] TraceContext generate_trace();

/// @brief 解析 W3C traceparent 头，格式为 "00-<32hex>-<16hex>-<2hex>"。
/// @param header 头部内容；更高版本允许追加字段，按规范忽略。
/// @param out 解析结果；仅在返回 true 时被写入。
/// @return 是否解析成功。version / 长度 / 字符 / 全 0 任一不符即返回 false，
///         调用方应按「无 trace」处理（不输出链路字段，而不是报错）。
bool parse_traceparent(std::string_view header, TraceContext& out);

/// @brief 生成传给下游的 traceparent 头。
/// @param ctx 链路上下文。
/// @return 头部字符串。四个字段任一非法（长度 / 字符 / 全 0）即返回空串——
///         不做任何默认值替换，避免发出「看似合法但语义被改过」的头。
[[nodiscard]] std::string make_traceparent(const TraceContext& ctx);

}  // namespace logger
