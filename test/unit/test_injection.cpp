#include <gtest/gtest.h>
#include <memory>
#include <stdexcept>
#include <string>

#include "logger/logger.h"
#include "logger/utiils.h"

#include "test_helpers.h"

// M6：日志注入防护 —— 一条记录只占一行，控制字符一律转义

namespace {

// 一条记录必须恰好一行：只在末尾有一个换行
void expect_single_line(const std::string& line) {
  ASSERT_FALSE(line.empty());
  EXPECT_EQ(line.back(), '\n');
  EXPECT_EQ(line.find('\n'), line.size() - 1) << "正文里出现了裸换行: " << line;
}

}  // namespace

// ===== text_escape =====

TEST(TextEscapeTest, EscapesControlCharacters) {
  EXPECT_EQ(text_escape("a\nb"), "a\\nb");
  EXPECT_EQ(text_escape("a\rb"), "a\\rb");
  EXPECT_EQ(text_escape("a\tb"), "a\\tb");
  EXPECT_EQ(text_escape("a\x1b[31m"), "a\\x1b[31m");  // ANSI 起始序列
  EXPECT_EQ(text_escape(std::string("a\0b", 3)), "a\\x00b");
  EXPECT_EQ(text_escape("a\x7f"), "a\\x7f");
}

TEST(TextEscapeTest, EscapesBackslashSoResultIsReversible) {
  // 反斜杠必须一起转义，否则无法区分「数据里就是反斜杠+n」与「转义后的换行」
  EXPECT_EQ(text_escape("a\\nb"), "a\\\\nb");
  EXPECT_NE(text_escape("a\nb"), text_escape("a\\nb"));
}

TEST(TextEscapeTest, LeavesPrintableTextUntouched) {
  EXPECT_EQ(text_escape("plain text 123"), "plain text 123");
  EXPECT_EQ(text_escape("中文 emoji 🙂"), "中文 emoji 🙂");
  EXPECT_EQ(text_escape(""), "");
}

TEST(TextEscapeTest, AppendMatchesReturningVersion) {
  std::string out = "head:";
  append_text_escaped("a\nb", out);
  EXPECT_EQ(out, "head:" + text_escape("a\nb"));
}

// ===== 与 Logger 的接线 =====

class InjectionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    Logger::get_instance().set_config(LogConfig{});
    sink_ = std::make_shared<CapturingSink>();
    Logger::get_instance().add_sink(sink_);
  }

  void use_json() {
    LogConfig cfg;
    cfg.format = LogFormat::JSON;
    Logger::get_instance().set_config(cfg);
  }

  std::string only_message() {
    EXPECT_EQ(sink_->size(), 1u);
    return sink_->messages().empty() ? std::string{} : sink_->messages().front();
  }

  std::shared_ptr<CapturingSink> sink_;
};

TEST_F(InjectionTest, NewlineInArgumentCannotForgeALogLine) {
  // 攻击载荷：伪造一条格式完全合法的 ERROR 记录
  const char* evil = "tom\n2026-01-01T00:00:00.000[ERROR][fake.cpp:1][hack]injected line\n[done]";
  Logger::get_instance().info("user login name={}", evil);

  const std::string line = only_message();
  expect_single_line(line);                                               // 只占一行
  EXPECT_NE(line.find("\\n2026-01-01T00:00:00.000"), std::string::npos);  // 换行被转义
  EXPECT_EQ(line.find("\n2026-01-01"), std::string::npos);
}

TEST_F(InjectionTest, AnsiAndControlCharactersAreEscaped) {
  Logger::get_instance().info("in:{}", "\x1b[31mRED\x1b[0m\rX\tY");

  const std::string line = only_message();
  expect_single_line(line);
  EXPECT_NE(line.find("\\x1b[31m"), std::string::npos);  // ANSI 不再生效
  EXPECT_EQ(line.find('\x1b'), std::string::npos);
  EXPECT_NE(line.find("\\r"), std::string::npos);
  EXPECT_NE(line.find("\\t"), std::string::npos);
}

TEST_F(InjectionTest, NewlineInFieldValueIsEscaped) {
  Logger::get_instance().info("m", KV("user_input", "a\nb"));

  const std::string line = only_message();
  expect_single_line(line);
  EXPECT_NE(line.find("user_input=a\\nb"), std::string::npos);
}

TEST_F(InjectionTest, NewlineInFieldKeyIsEscaped) {
  Logger::get_instance().info("m", KV(std::string_view("a\nb"), 1));

  const std::string line = only_message();
  expect_single_line(line);
  EXPECT_NE(line.find("a\\nb=1"), std::string::npos);
}

TEST_F(InjectionTest, NewlineInFormatStringIsNeutralized) {
  // 一条记录一行的约束对格式串同样生效：真换行也被转义
  LOG_INFO("line1\nline2");

  const std::string line = only_message();
  expect_single_line(line);
  EXPECT_NE(line.find("line1\\nline2"), std::string::npos);
}

TEST_F(InjectionTest, LiteralBackslashNStaysDistinguishable) {
  LOG_INFO("A: a\\nb");  // 格式串里是反斜杠 + n 两个字符
  const std::string literal = only_message();

  Logger::get_instance().info("B: a\nb");  // 参数里是真换行
  const std::string real = sink_->messages().back();

  // 字面量被转义成两个反斜杠，真换行转义成一个 —— 两者可区分、可还原
  EXPECT_NE(literal.find("A: a\\\\nb"), std::string::npos);
  EXPECT_NE(real.find("B: a\\nb"), std::string::npos);
  EXPECT_NE(literal, real);
}

TEST_F(InjectionTest, ErrorChainStaysOnOneLine) {
  try {
    try {
      throw std::runtime_error("inner");
    } catch (...) {
      std::throw_with_nested(std::logic_error("outer"));
    }
  } catch (const std::exception& e) {
    LOG_EXCEPTION("db failed", e);
  }

  const std::string line = only_message();
  expect_single_line(line);  // error_chain 自带的换行也必须收住
  EXPECT_NE(line.find("outer\\n  caused by: std::runtime_error: inner"), std::string::npos);
}

TEST_F(InjectionTest, StacktraceStaysOnOneLine) {
  LOG_FATAL("boom");  // 默认自动附堆栈
  expect_single_line(only_message());
}

TEST_F(InjectionTest, JsonOutputIsAlsoSingleLine) {
  use_json();
  Logger::get_instance().info("in:{}", "a\nb\r\x1b[0m");

  const std::string line = only_message();
  expect_single_line(line);
  EXPECT_EQ(line.front(), '{');
  EXPECT_NE(line.find("\\n"), std::string::npos);
  EXPECT_NE(line.find("\\u001b"), std::string::npos);
}

TEST_F(InjectionTest, ReadableTextIsNotDisturbed) {
  Logger::get_instance().info("order created", KV("order_id", "ORD-1001"), KV("amount", 99.5));

  const std::string line = only_message();
  expect_single_line(line);
  EXPECT_NE(line.find("order created"), std::string::npos);
  EXPECT_NE(line.find("order_id=ORD-1001"), std::string::npos);
  EXPECT_NE(line.find("amount=99.5"), std::string::npos);  // 小数点没被当成控制字符
}
