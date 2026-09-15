// 从「装好的包」而不是源码树使用本库：包含、链接、宏、结构化字段、版本号各验一遍。
// 版本号那条还是两处一致性的活体检查 —— 装出来的版本宏和 find_package 要的版本号同源。
#include <cstdio>
#include <logger/logger.h>
#include <memory>
#include <string>

using namespace logger;

int main() {
  Logger::get_instance().add_sink(std::make_shared<ConsoleSink>());
  LOG_INFO("consumer ok, logger {}", LOG_VERSION);
  Logger::get_instance().info("paid", KV("amount", 99.5));
  Logger::get_instance().flush_all();

  // 打印到 stdout 以便 CI 断言；真正的验证是「能编过 + 能跑起来」
  std::printf("LOG_VERSION=%s CODE=%d\n", LOG_VERSION, LOG_VERSION_CODE);
  return std::string(LOG_VERSION) == "1.0.0" ? 0 : 1;
}
