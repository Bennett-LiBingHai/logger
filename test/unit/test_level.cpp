#include <gtest/gtest.h>
#include <string>

#include "logger/level.h"

// M1：日志级别定义与字符串映射
TEST(LogLevelTest, ValuesAreStrictlyIncreasing) {
  // 级别过滤依赖该顺序：logLevel < config_.log_level 时被忽略
  EXPECT_TRUE(LogLevel::TRACE < LogLevel::DEBUG);
  EXPECT_TRUE(LogLevel::DEBUG < LogLevel::INFO);
  EXPECT_TRUE(LogLevel::INFO < LogLevel::WARN);
  EXPECT_TRUE(LogLevel::WARN < LogLevel::ERROR);
  EXPECT_TRUE(LogLevel::ERROR < LogLevel::FATAL);
  EXPECT_TRUE(LogLevel::FATAL < LogLevel::OFF);
}

TEST(LogLevelTest, ToStringReturnsExpectedLabel) {
  EXPECT_EQ(std::string(to_string(LogLevel::TRACE)), "TRACE");
  EXPECT_EQ(std::string(to_string(LogLevel::DEBUG)), "DEBUG");
  EXPECT_EQ(std::string(to_string(LogLevel::INFO)), "INFO");
  EXPECT_EQ(std::string(to_string(LogLevel::WARN)), "WARN");
  EXPECT_EQ(std::string(to_string(LogLevel::ERROR)), "ERROR");
  EXPECT_EQ(std::string(to_string(LogLevel::FATAL)), "FATAL");
  EXPECT_EQ(std::string(to_string(LogLevel::OFF)), "OFF");
}
