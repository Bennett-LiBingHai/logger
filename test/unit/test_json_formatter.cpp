#include <chrono>
#include <gtest/gtest.h>
#include <string>
#include <thread>
#include <vector>

#include "logger/field.h"
#include "logger/formatter/json_formatter.h"
#include "logger/record.h"

namespace {
Record make_record(const std::string& content, std::vector<Field> fields) {
  Record r;
  r.time = std::chrono::system_clock::now();
  r.log_level = LogLevel::INFO;
  r.content = content;
  r.thread_id = std::this_thread::get_id();
  r.file = "test.cpp";
  r.line = 42;
  r.func = "test_func";
  r.fields = std::move(fields);
  return r;
}
}  // namespace

// M2：JSON 字段类型保留、顺序、转义、级别小写
TEST(JsonFormatterTest, EmitsTypedFieldsInOrder) {
  auto r = make_record("order created",
                       {KV("order_id", "ORD-1001"), KV("user_id", 2001), KV("amount", 99.5)});
  FormatResult result = JsonFormatter::format(r, LogConfig{}, true);
  const std::string& j = result.formatted_msg;
  // 类型保留：字符串带引号，数字不带
  EXPECT_NE(j.find("\"order_id\": \"ORD-1001\""), std::string::npos);
  EXPECT_NE(j.find("\"user_id\": 2001"), std::string::npos);
  EXPECT_NE(j.find("\"amount\": 99.5"), std::string::npos);
  // 顺序：按调用顺序
  EXPECT_LT(j.find("order_id"), j.find("user_id"));
  EXPECT_LT(j.find("user_id"), j.find("amount"));
}

TEST(JsonFormatterTest, LevelIsLowercase) {
  auto r = make_record("m", {});
  FormatResult result = JsonFormatter::format(r, LogConfig{}, true);
  EXPECT_NE(result.formatted_msg.find("\"level\": \"info\""), std::string::npos);
}

TEST(JsonFormatterTest, EscapesSpecialChars) {
  auto r = make_record("line\nbreak", {KV("k", "a\"b")});
  FormatResult result = JsonFormatter::format(r, LogConfig{}, true);
  const std::string& j = result.formatted_msg;
  EXPECT_NE(j.find("\\n"), std::string::npos);
  EXPECT_NE(j.find("\\\""), std::string::npos);
}

TEST(JsonFormatterTest, LessOmitsDebugFields) {
  auto r = make_record("m", {});
  FormatResult result = JsonFormatter::format(r, LogConfig{}, true);
  EXPECT_EQ(result.formatted_msg.find("\"file\""), std::string::npos);

  FormatResult full = JsonFormatter::format(r, LogConfig{}, false);
  EXPECT_NE(full.formatted_msg.find("\"file\""), std::string::npos);
}
