#include <logger/logger.h>
#include <logger/sink/console_sink.h>
#include <memory>

int main() {
  // 挂一个控制台 Sink：Error 及以上 → stderr，其余 → stdout
  Logger::get_instance().add_sink(std::make_shared<ConsoleSink>());

  // 默认级别 TRACE，各级别都会输出
  LOG_TRACE("trace message");
  LOG_DEBUG("debug message");
  LOG_INFO("info message");
  LOG_WARN("warn message");
  LOG_ERROR("error message");
  LOG_FATAL("fatal message");

  // {} 风格位置参数格式化
  LOG_INFO("user {} login, name={}", 1001, "tom");

  // 结构化字段（文本格式输出 key=value）
  Logger::get_instance().info("order created", KV("order_id", "ORD-1001"), KV("amount", 99.5));

  // 切换 JSON 输出
  LogConfig cfg;
  cfg.format = LogFormat::JSON;
  Logger::get_instance().set_config(cfg);
  Logger::get_instance().info("payment succeeded", KV("trace_id", "abc123"), KV("amount", 200));

  // 调高全局级别：DEBUG 及以下被过滤
  cfg.format = LogFormat::TEXT;
  cfg.log_level = LogLevel::INFO;
  Logger::get_instance().set_config(cfg);

  LOG_DEBUG("this debug line is filtered out");
  LOG_INFO("this info line is kept");

  Logger::get_instance().flush_all();
  return 0;
}
