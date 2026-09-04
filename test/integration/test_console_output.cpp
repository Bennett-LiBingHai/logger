#include <gtest/gtest.h>
#include <memory>
#include "logger/logger.h"
#include "logger/sink/console_sink.h"

// 手动观察用：验证 ConsoleSink 按级别路由。
//   默认 errlevel = ERROR：Error/Fatal → stderr，Trace/Debug/Info/Warn → stdout。
// 观察方式（在 build 目录下）：
//   ./test_console                 # 直接看终端，注意 [ERROR]/[FATAL] 行走向
//   ./test_console 2>/dev/null     # 只保留 stdout：应无 Error/Fatal
//   ./test_console 2>stderr.log    # stderr 单独落盘：应只含 Error/Fatal
TEST(ConsoleOutputTest, ManualObserveStderrRouting) {
    Logger::get_instance().set_config(LogConfig{});
    Logger::get_instance().add_sink(std::make_shared<ConsoleSink>());

    LOG_TRACE("trace message");
    LOG_DEBUG("debug message");
    LOG_INFO("info message");
    LOG_WARN("warn message");
    LOG_ERROR("error message");
    LOG_FATAL("fatal message");

    Logger::get_instance().flush_all();
    SUCCEED();
}
