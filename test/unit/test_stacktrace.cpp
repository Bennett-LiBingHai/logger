#include <algorithm>
#include <cstddef>
#include <gtest/gtest.h>
#include <memory>
#include <string>

#include "logger/logger.h"
#include "logger/stacktrace.h"

#include "test_helpers.h"

// M5：堆栈信息 —— 采集、符号化、预算截断、与 Logger 的接线
//
// 注意：capture_in_named_frame 必须放在全局作用域（外部链接）。
// 匿名命名空间 / static 函数是内部链接，不进 .dynsym，dladdr 解不出符号名。
StackTrace capture_in_named_frame() {
  return StackTrace::capture();
}

// 专门撑出多帧调用链，用于验证预算截断
StackTrace capture_deep(std::size_t depth, std::size_t budget) {
  if (depth == 0)
    return StackTrace::capture(1, 32, budget);
  return capture_deep(depth - 1, budget);
}

TEST(StackTraceTest, CapturesFrames) {
  const StackTrace st = capture_in_named_frame();
  EXPECT_FALSE(st.empty());
  EXPECT_FALSE(st.str().empty());
}

TEST(StackTraceTest, SymbolizesCurrentFunction) {
  // 依赖 -rdynamic（logger 的 CMake 目标以 INTERFACE 方式传递）
  const std::string s = capture_in_named_frame().str();
  EXPECT_NE(s.find("capture_in_named_frame"), std::string::npos);
}

TEST(StackTraceTest, SkipRemovesExactlyOneFrame) {
  // 用不限预算 + 数帧数来判断，避开两个坑：
  //   1. 不能比总长度：跳掉一帧会腾出预算，后面可能多排进一帧，总长反而更长
  //   2. 不能断言"skip=1 后不含 StackTrace::capture"：ASan/TSan 会插入自己的
  //      backtrace 拦截帧，capture 会落到第 1 帧，那属于工具链差异
  const StackTrace with_own = StackTrace::capture(0, 32, 0);
  const StackTrace without_own = StackTrace::capture(1, 32, 0);
  const auto frames = [](const StackTrace& st) {
    return std::count(st.str().begin(), st.str().end(), '\n');
  };
  EXPECT_EQ(frames(without_own), frames(with_own) - 1);
  EXPECT_NE(with_own.str().find("StackTrace::capture"), std::string::npos);
}

TEST(StackTraceTest, SymbolizationIsCachedAcrossCalls) {
  const StackTrace st = capture_in_named_frame();
  EXPECT_EQ(&st.str(), &st.str());  // 第二次返回同一个缓存对象
}

TEST(StackTraceTest, TextEncodingIsSingleLine) {
  const StackTrace st = capture_in_named_frame();
  std::string out;
  encode(st, out, false);
  EXPECT_EQ(out.find('\n'), std::string::npos);  // 文本日志按行解析，必须折叠换行
  EXPECT_NE(out.find("capture_in_named_frame"), std::string::npos);
}

TEST(StackTraceTest, JsonEncodingIsQuotedAndEscaped) {
  const StackTrace st = capture_in_named_frame();
  std::string out;
  encode(st, out, true);
  ASSERT_FALSE(out.empty());
  EXPECT_EQ(out.front(), '"');
  EXPECT_EQ(out.back(), '"');
  EXPECT_EQ(out.find('\n'), std::string::npos);  // 真实换行已转义
  EXPECT_NE(out.find("\\n"), std::string::npos);
}

TEST(StackTraceTest, BudgetBoundsRenderedSize) {
  constexpr std::size_t kBudget = 120;
  const StackTrace st = capture_deep(6, kBudget);
  EXPECT_LE(st.str().size(), kBudget);
}

TEST(StackTraceTest, TinyBudgetMarksDroppedFrames) {
  // 预算小到一帧都放不下：应只输出省略标记，而不是超预算或输出半帧
  const StackTrace st = StackTrace::capture(1, 32, 40);
  const std::string s = st.str();
  EXPECT_NE(s.find("frames)"), std::string::npos);
  EXPECT_LE(s.size(), 40u);
}

TEST(StackTraceTest, ZeroBudgetDropsNothing) {
  // 0 = 不限预算：不因预算丢帧，也就不该出现省略标记
  // （同样不能靠比长度来判断——受限的那份可能因为帧短而恰好不触发截断）
  EXPECT_EQ(StackTrace::capture(1, 32, 0).str().find("(+"), std::string::npos);
}

TEST(StackTraceTest, ZeroDepthYieldsEmpty) {
  EXPECT_TRUE(StackTrace::capture(1, 0).empty());
}

// ===== 与 Logger 的接线 =====

class StackTraceLogTest : public ::testing::Test {
 protected:
  void SetUp() override {
    Logger::get_instance().set_config(LogConfig{});
    sink_ = std::make_shared<CapturingSink>();
    Logger::get_instance().add_sink(sink_);
  }

  std::string only_message() {
    EXPECT_EQ(sink_->size(), 1u);
    return sink_->messages().empty() ? std::string{} : sink_->messages().front();
  }

  std::shared_ptr<CapturingSink> sink_;
};

TEST_F(StackTraceLogTest, ErrorDoesNotAttachByDefault) {
  LOG_ERROR("boom");
  EXPECT_EQ(only_message().find("stacktrace="), std::string::npos);
}

TEST_F(StackTraceLogTest, FatalAttachesByDefault) {
  LOG_FATAL("boom");
  EXPECT_NE(only_message().find("stacktrace="), std::string::npos);
}

TEST_F(StackTraceLogTest, OffModeDisablesEvenFatal) {
  LogConfig cfg;
  cfg.stacktrace = StackTraceMode::OFF;
  Logger::get_instance().set_config(cfg);

  LOG_FATAL("boom");
  EXPECT_EQ(only_message().find("stacktrace="), std::string::npos);
}

TEST_F(StackTraceLogTest, AlwaysModeAttachesToLowerLevels) {
  LogConfig cfg;
  cfg.stacktrace = StackTraceMode::ALWAYS;
  Logger::get_instance().set_config(cfg);

  LOG_INFO("hello");
  EXPECT_NE(only_message().find("stacktrace="), std::string::npos);
}

TEST_F(StackTraceLogTest, ExplicitKvWorksRegardlessOfMode) {
  // 显式 KV 就是普通字段，不受自动采集策略约束
  LOG_ERROR("boom", KV("stacktrace", StackTrace::capture()));
  EXPECT_NE(only_message().find("stacktrace="), std::string::npos);
}

TEST_F(StackTraceLogTest, FatalLineStaysWithinMaxLogItemSize) {
  // 堆栈有独立预算，不应把整条日志顶到被 max_log_item_size 硬切
  LogConfig cfg;
  cfg.max_log_item_size = 1024;
  Logger::get_instance().set_config(cfg);

  LOG_FATAL("boom");
  const std::string line = only_message();
  EXPECT_LT(line.size(), cfg.max_log_item_size);
  EXPECT_EQ(line.back(), '\n');  // 行结构完好（硬切会吃掉结尾换行）
}
