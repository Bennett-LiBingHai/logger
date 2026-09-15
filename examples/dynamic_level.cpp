#include <logger/logger.h>
#include <logger/sink/console_sink.h>
#include <memory>

using namespace logger;  // 库的公共符号都在 logger:: 下

// 运行期调整级别：接到 HTTP / SIGHUP / 配置中心里，就是一行 set_level()
int main() {
  Logger::get_instance().add_sink(std::make_shared<ConsoleSink>());

  LOG_DEBUG("1. 默认 TRACE —— debug 可见");
  LOG_INFO("1. info 可见");

  // 生产上通常由管理接口触发，这里直接调用演示
  Logger::get_instance().set_level(LogLevel::WARN);
  LOG_DEBUG("2. 抬高到 WARN 后 —— debug 被过滤");
  LOG_INFO("2. info 也被过滤");
  LOG_WARN("2. warn 可见");
  LOG_ERROR("2. error 可见");

  // 排障时临时放开
  Logger::get_instance().set_level(LogLevel::TRACE);
  LOG_DEBUG("3. 放开后 —— debug 又能看到");
  return 0;
}
