#include <exception>
#include <gtest/gtest.h>
#include <memory>
#include <stdexcept>
#include <string>

#include "logger/error.h"
#include "logger/logger.h"

#include "test_helpers.h"

// M5：错误记录 —— 异常提取与 LOG_EXCEPTION 自动展开

namespace {

// 抛出带嵌套链的异常：外层 logic_error 包裹内层 runtime_error
void throw_nested() {
  try {
    throw std::runtime_error("inner failure");
  } catch (...) {
    std::throw_with_nested(std::logic_error("outer failure"));
  }
}

}  // namespace

TEST(ExceptionInfoTest, ExtractsMessageAndType) {
  std::runtime_error e("boom");
  const ExceptionInfo info = extract_exception(e);
  EXPECT_EQ(info.message, "boom");
  EXPECT_EQ(info.type, "std::runtime_error");
  EXPECT_EQ(info.chain, "std::runtime_error: boom");
  EXPECT_FALSE(info.has_nested);
}

TEST(ExceptionInfoTest, TypeIsDynamicNotStatic) {
  // 以基类引用传入，也应取到动态类型（std::exception 有多态性）
  const std::overflow_error e("too big");
  const std::exception& base = e;
  EXPECT_EQ(extract_exception(base).type, "std::overflow_error");
}

TEST(ExceptionInfoTest, NestedChainKeepsOrderAndHidesWrapper) {
  try {
    throw_nested();
  } catch (const std::exception& e) {
    const ExceptionInfo info = extract_exception(e);
    EXPECT_TRUE(info.has_nested);
    // message/type 取最内层（根因）
    EXPECT_EQ(info.message, "inner failure");
    EXPECT_EQ(info.type, "std::runtime_error");
    // 链为「外层 → caused by → 内层」
    EXPECT_EQ(info.chain,
              "std::logic_error: outer failure\n  caused by: std::runtime_error: inner failure");
    // 不得泄漏 throw_with_nested 的内部包装类型
    EXPECT_EQ(info.chain.find("_Nested_exception"), std::string::npos);
  }
}

TEST(ExceptionInfoTest, NullExceptionPtrYieldsPlaceholder) {
  const ExceptionInfo info = extract_exception(std::exception_ptr{});
  EXPECT_FALSE(info.message.empty());
  EXPECT_FALSE(info.has_nested);
}

TEST(ExceptionInfoTest, ExceptionPtrResolvesDynamicType) {
  std::exception_ptr ep;
  try {
    throw std::range_error("via ptr");
  } catch (...) {
    ep = std::current_exception();
  }
  const ExceptionInfo info = extract_exception(ep);
  EXPECT_EQ(info.message, "via ptr");
  EXPECT_EQ(info.type, "std::range_error");
}

// ===== LOG_EXCEPTION 输出 =====

class ErrorLogTest : public ::testing::Test {
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

TEST_F(ErrorLogTest, EmitsErrorAndTypeWithoutChain) {
  const std::runtime_error e("db down");
  LOG_EXCEPTION("db query failed", e);

  const std::string line = only_message();
  EXPECT_NE(line.find("[ERROR]"), std::string::npos);
  EXPECT_NE(line.find("db query failed"), std::string::npos);
  EXPECT_NE(line.find("error=db down"), std::string::npos);
  EXPECT_NE(line.find("error_type=std::runtime_error"), std::string::npos);
  EXPECT_EQ(line.find("error_chain="), std::string::npos);  // 无嵌套则不输出该字段
}

TEST_F(ErrorLogTest, EmitsChainWhenNested) {
  try {
    throw_nested();
  } catch (const std::exception& e) {
    LOG_EXCEPTION("db query failed", e);
  }

  const std::string line = only_message();
  EXPECT_NE(line.find("error=inner failure"), std::string::npos);
  EXPECT_NE(line.find("error_chain="), std::string::npos);
  EXPECT_NE(line.find("caused by: std::runtime_error"), std::string::npos);
}

TEST_F(ErrorLogTest, AcceptsExtraFields) {
  const std::runtime_error e("x");
  LOG_EXCEPTION("msg", e, KV("retry", 3), KV("request_id", "req-7"));

  const std::string line = only_message();
  EXPECT_NE(line.find("retry=3"), std::string::npos);
  EXPECT_NE(line.find("request_id=req-7"), std::string::npos);
  EXPECT_NE(line.find("error=x"), std::string::npos);
}

TEST_F(ErrorLogTest, AcceptsExceptionPtr) {
  std::exception_ptr ep;
  try {
    throw std::logic_error("from ptr");
  } catch (...) {
    ep = std::current_exception();
  }
  LOG_EXCEPTION("failed", ep);

  const std::string line = only_message();
  EXPECT_NE(line.find("error=from ptr"), std::string::npos);
  EXPECT_NE(line.find("error_type=std::logic_error"), std::string::npos);
}

TEST_F(ErrorLogTest, StructuredInterfaceOmitsFileInfo) {
  // exception() 直调不带文件/行/函数 → less 分支，输出不含 [file:line]
  const std::runtime_error e("x");
  Logger::get_instance().exception("failed", e);

  const std::string line = only_message();
  EXPECT_NE(line.find("error=x"), std::string::npos);
  EXPECT_EQ(line.find("test_error.cpp"), std::string::npos);
}

TEST_F(ErrorLogTest, RespectsLevelFiltering) {
  LogConfig cfg;
  cfg.log_level = LogLevel::FATAL;  // ERROR 及以上才输出，ERROR < FATAL
  Logger::get_instance().set_config(cfg);

  const std::runtime_error e("x");
  LOG_EXCEPTION("filtered out", e);
  EXPECT_EQ(sink_->size(), 0u);
}
