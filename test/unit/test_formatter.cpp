#include <gtest/gtest.h>
#include <chrono>
#include <regex>
#include <thread>
#include "logger/record.h"
#include "logger/config.h"
#include "logger/formatter/text_formatter.h"
#include "test_helpers.h"

namespace {
Record make_record(LogLevel level, const std::string& content) {
    Record r;
    r.time = std::chrono::system_clock::now();
    r.log_level = level;
    r.content = content;
    r.thread_id = std::this_thread::get_id();
    r.file = "test.cpp";
    r.line = 42;
    r.func = "test_func";
    return r;
}
}  // namespace

// M1：文本输出格式稳定
TEST(TextFormatterTest, FormatsAllMessageFields) {
    const FormatResult result =
        TextFormatter::format(make_record(LogLevel::INFO, "hello world"), LogConfig{});
    const std::string& line = result.formatted_msg;
    EXPECT_EQ(result.level, LogLevel::INFO);
    EXPECT_TRUE(matches_log_line(line));  // 完整结构
    EXPECT_NE(line.find("[INFO]"), std::string::npos);
    EXPECT_NE(line.find("[test.cpp:42]"), std::string::npos);
    EXPECT_NE(line.find("hello world"), std::string::npos);
}

TEST(TextFormatterTest, EmbedsThreadId) {
    const FormatResult result =
        TextFormatter::format(make_record(LogLevel::DEBUG, "msg"), LogConfig{});
    const std::string& line = result.formatted_msg;
    // [LEVEL] 之后必须紧跟 [thread_id]
    std::smatch m;
    EXPECT_TRUE(std::regex_search(line, m, std::regex(R"(\[[A-Z]+\]\[([^\]]+)\])")));
    ASSERT_EQ(m.size(), 2u);
    EXPECT_FALSE(m[1].str().empty());
}

TEST(TextFormatterTest, UsesConfiguredLevelLabel) {
    const FormatResult result =
        TextFormatter::format(make_record(LogLevel::ERROR, "boom"), LogConfig{});
    const std::string& line = result.formatted_msg;
    EXPECT_EQ(result.level, LogLevel::ERROR);
    EXPECT_NE(line.find("[ERROR]"), std::string::npos);
}
