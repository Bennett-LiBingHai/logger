#include <logger/logger.h>
#include <logger/sink/console_sink.h>
#include <logger/trace.h>
#include <memory>

using namespace logger;  // 库的公共符号都在 logger:: 下

// Trace 集成：从上游 traceparent 接续链路，作用域内日志自动带 trace_id / span_id
int main() {
  Logger::get_instance().add_sink(std::make_shared<ConsoleSink>());

  LogConfig cfg;
  cfg.format = LogFormat::JSON;  // 结构化输出，trace 字段可被下游直接检索
  Logger::get_instance().set_config(cfg);

  // 模拟从上游 HTTP 头拿到的 traceparent；拿不到就自己起一条链路
  TraceContext trace;
  if (!parse_traceparent("00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01", trace))
    trace = generate_trace();

  {
    // 作用域内所有日志自动带上下文字段
    ContextScope ctx{KV("trace_id", trace.trace_id), KV("span_id", trace.span_id)};
    LOG_INFO("handling request");
    LOG_INFO("query done rows={}", 42);
  }

  // 出作用域后不再带；要往下游传就自己塞进 HTTP 头
  LOG_INFO("traceparent for downstream: {}", make_traceparent(trace));
  return 0;
}
