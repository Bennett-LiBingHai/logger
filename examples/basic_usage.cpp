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

  // printf 风格变参格式化
  LOG_INFO("user %d login, name=%s", 1001, "tom");

  // 调高全局级别：DEBUG 及以下被过滤
  LogConfig cfg;
  cfg.log_level = LogLevel::INFO;
  Logger::get_instance().set_config(cfg);

  LOG_DEBUG("this debug line is filtered out");
  LOG_INFO("this info line is kept");

  Logger::get_instance().flush_all();
  return 0;
}
