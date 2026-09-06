#include <gtest/gtest.h>
#include <string>

#include "logger/format.h"

// M2：{} 位置参数格式化（消息正文）
TEST(FormatTest, InterpolatesPositionalArgs) {
  EXPECT_EQ(format("user {} login", 1001), "user 1001 login");
  EXPECT_EQ(format("{} + {} = {}", 1, 2, 3), "1 + 2 = 3");
  EXPECT_EQ(format("name={}", std::string("tom")), "name=tom");
}

TEST(FormatTest, NoPlaceholderReturnsAsIs) {
  EXPECT_EQ(format("plain message"), "plain message");
}

TEST(FormatTest, MissingArgKeepsPlaceholder) {
  EXPECT_EQ(format("a={} b={}", 1), "a=1 b={}");
}

TEST(FormatTest, ExtraArgsIgnored) {
  EXPECT_EQ(format("x={}", 1, 2, 3), "x=1");
}

TEST(FormatTest, EmptyFmt) {
  EXPECT_EQ(format("", 1), "");
}
