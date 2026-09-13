#include <gtest/gtest.h>

#include "logger/config.h"

// M1：默认配置
TEST(LogConfigTest, DefaultsAllowAllLevelsAndIso8601) {
  LogConfig config;
  EXPECT_EQ(config.log_level, LogLevel::TRACE);
  EXPECT_EQ(config.max_record_size, 1024u);
  EXPECT_EQ(config.max_message_length, 0u);  // 0 = 不限
  EXPECT_EQ(config.max_field_length, 0u);
  EXPECT_EQ(config.max_stacktrace_length, 512u);
  EXPECT_EQ(config.time_format, TimeFormat::ISO8601);
  EXPECT_FALSE(config.use_utc_time);
  EXPECT_EQ(config.format, LogFormat::TEXT);
  EXPECT_FALSE(config.async);
}
