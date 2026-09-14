#include <cmath>
#include <gtest/gtest.h>
#include <limits>
#include <memory>
#include <string>

#include "logger/detail/utils.h"
#include "logger/logger.h"

#include "test_helpers.h"

// M6：日志长度限制 —— UTF-8 安全截断、消息上限、字段上限、整条预算

// ===== truncate_utf8 =====

TEST(TruncateUtf8Test, LeavesShortStringUntouched) {
  std::string s = "abc";
  truncate_utf8(s, 10);
  EXPECT_EQ(s, "abc");
  truncate_utf8(s, 3);
  EXPECT_EQ(s, "abc");  // 恰好等于上限也不截断
}

TEST(TruncateUtf8Test, AppendsMarkerWithinLimit) {
  std::string s = "0123456789";
  truncate_utf8(s, 5);
  EXPECT_EQ(s, "01...");
  EXPECT_EQ(s.size(), 5u);  // 标记占用预算内
}

TEST(TruncateUtf8Test, NeverSplitsMultiByteCodePoint) {
  // 每个汉字 3 字节；截到 8 时不能切在码点中间
  std::string s = "中文中文";  // 12 字节
  truncate_utf8(s, 8);
  EXPECT_LE(s.size(), 8u);
  EXPECT_EQ(s, "中...");  // 退到完整码点边界

  // 逐字节校验：不产生非法 UTF-8 起始/续字节组合
  for (std::size_t i = 0; i < s.size(); ++i) {
    const auto c = static_cast<unsigned char>(s[i]);
    if ((c & 0xC0) == 0x80)
      EXPECT_NE(c & 0xC0, 0xC0) << "续字节出现在码点起始位置";
    EXPECT_NE(static_cast<int>(c), 0xFF);
  }
}

TEST(TruncateUtf8Test, TinyLimitYieldsEmptyOrBareMarker) {
  std::string s = "abcdef";
  truncate_utf8(s, 2);
  EXPECT_EQ(s, "ab");  // 放不下 "..." 时只截断，不加标记

  s = "abcdef";
  truncate_utf8(s, 0);
  EXPECT_TRUE(s.empty());
}

// ===== 消息 / 字段上限 =====

class LimitsTest : public ::testing::Test {
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

TEST_F(LimitsTest, MessageLengthIsUnlimitedByDefault) {
  const std::string body(500, 'x');
  Logger::get_instance().info("{}", body);
  EXPECT_NE(only_message().find(body), std::string::npos);
}

TEST_F(LimitsTest, MessageIsTruncatedToConfiguredLimit) {
  LogConfig cfg;
  cfg.max_message_length = 32;
  Logger::get_instance().set_config(cfg);

  const std::string body(500, 'x');
  Logger::get_instance().info("{}", body);

  const std::string line = only_message();
  EXPECT_EQ(line.find(body), std::string::npos);  // 全文不得出现
  EXPECT_NE(line.find("..."), std::string::npos);
}

TEST_F(LimitsTest, FieldLengthIsUnlimitedByDefault) {
  const std::string value(500, 'y');
  Logger::get_instance().info("m", KV("blob", value));
  EXPECT_NE(only_message().find(value), std::string::npos);
}

TEST_F(LimitsTest, FieldValueIsTruncatedToConfiguredLimit) {
  LogConfig cfg;
  cfg.max_field_length = 16;
  Logger::get_instance().set_config(cfg);

  const std::string value(500, 'y');
  Logger::get_instance().info("m", KV("blob", value));

  const std::string line = only_message();
  EXPECT_EQ(line.find(value), std::string::npos);
  EXPECT_NE(line.find("blob=yyyyyyyyyyyyy..."), std::string::npos);
}

TEST_F(LimitsTest, FieldLimitMeasuresInTargetFormat) {
  // 同一个值在不同格式下长度不同：NaN 在 text 下是 "nan"(3)，JSON 下是 "null"(4)
  const double nan = std::numeric_limits<double>::quiet_NaN();

  LogConfig text_cfg;
  text_cfg.max_field_length = 3;
  Logger::get_instance().set_config(text_cfg);
  Logger::get_instance().info("m", KV("v", nan));
  EXPECT_NE(only_message().find("v=nan"), std::string::npos);  // 3 字节，未超限

  LogConfig json_cfg;
  json_cfg.format = LogFormat::JSON;
  json_cfg.max_field_length = 3;
  Logger::get_instance().set_config(json_cfg);
  Logger::get_instance().info("m", KV("v", nan));
  EXPECT_NE(sink_->messages().back().find("\"v\": \"...\""),
            std::string::npos);  // 4 字节超限，被截断
}

TEST_F(LimitsTest, MessageAndFieldLimitsWorkTogether) {
  LogConfig cfg;
  cfg.max_message_length = 24;
  cfg.max_field_length = 8;
  Logger::get_instance().set_config(cfg);

  Logger::get_instance().info("payload {}", std::string(200, 'a'),
                              KV("body", std::string(200, 'b')));

  const std::string line = only_message();
  EXPECT_EQ(line.find(std::string(200, 'a')), std::string::npos);
  EXPECT_EQ(line.find(std::string(200, 'b')), std::string::npos);
  EXPECT_NE(line.find("body=bbbbb..."), std::string::npos);
}
