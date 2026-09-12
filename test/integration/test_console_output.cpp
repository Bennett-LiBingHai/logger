#include <gtest/gtest.h>
#include <memory>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "logger/crash_handler.h"
#include "logger/logger.h"
#include "logger/sink/console_sink.h"

namespace {

// 外部链接的全局函数，addr2line 才能解析出名字
__attribute__((noinline)) void crash_c(int* p) {
  *p = 42;  // nullptr 解引用 → SIGSEGV
}

__attribute__((noinline)) void crash_b(int* p) {
  crash_c(p);
}

__attribute__((noinline)) void crash_a(int* p) {
  crash_b(p);
}

}  // namespace

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

// 手动观察崩溃现场：在子进程里触发 SIGSEGV。
// 父子共用 stderr，崩溃 dump 会直接打在终端上（gtest 本身仍然通过）。
//   ./test_console              # 终端里应出现 "=== CRASH ===" 段
//   ./test_console 2>crash.txt  # 落到文件里，再用 addr2line 解偏移
TEST(CrashHandlerTest, ManualObserveCrashDump) {
  // 只有 SIGSEGV 原本是默认动作时，本库才会接管它。
  // sanitizer（ASan/TSan）会先占住 SIGSEGV/SIGBUS/SIGFPE，此时崩溃由它处理，
  // 进程会死于它选择的信号（ASan 报告后走 abort → SIGABRT），观察不到我们的现场。
  struct sigaction prev {};
  ASSERT_EQ(::sigaction(SIGSEGV, nullptr, &prev), 0);
  if (prev.sa_handler != SIG_DFL)
    GTEST_SKIP() << "SIGSEGV 已被其它库接管（如 sanitizer），本用例跳过";

  ASSERT_TRUE(install_crash_handler());  // 空路径 → 写 stderr

  const pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    crash_a(nullptr);  // 子进程在这里崩溃，不会返回
    _exit(0);
  }

  int status = 0;
  ASSERT_EQ(::waitpid(pid, &status, 0), pid);
  uninstall_crash_handler();

  // 关键行为：记录完现场仍走默认动作，保留 core dump 语义
  EXPECT_TRUE(WIFSIGNALED(status));
  EXPECT_EQ(WTERMSIG(status), SIGSEGV);
}
