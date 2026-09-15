#include <gtest/gtest.h>
#include <memory>
#include <stdexcept>
#include <string>

#include "logger/logger.h"

#include "test_helpers.h"

using namespace logger;          // 库的公共符号
using namespace logger::detail;  // 白盒用例要直接构造 Record / Formatter 等内部类型

// M6：敏感字段脱敏 —— 默认脱敏函数、启用开关、类型保留、异常兜底

class MaskTest : public ::testing::Test {
 protected:
  void SetUp() override {
    Logger::get_instance().set_config(LogConfig{});
    sink_ = std::make_shared<CapturingSink>();
    Logger::get_instance().add_sink(sink_);
  }

  // 启用脱敏；masker 传 nullptr 表示沿用默认脱敏函数
  void enable_mask(LogConfig::MaskerFn masker = nullptr) {
    LogConfig cfg;
    cfg.enable_sensitive_field_mask = true;
    if (masker)
      cfg.sensitive_field_masker = std::move(masker);
    Logger::get_instance().set_config(cfg);
  }

  std::string only_message() {
    EXPECT_EQ(sink_->size(), 1u);
    return sink_->messages().empty() ? std::string{} : sink_->messages().front();
  }

  std::shared_ptr<CapturingSink> sink_;
};

TEST_F(MaskTest, DisabledByDefault) {
  Logger::get_instance().info("login", KV("password", "secret123"));
  EXPECT_NE(only_message().find("password=secret123"), std::string::npos);
}

TEST_F(MaskTest, DefaultMaskerHidesKnownKeys) {
  enable_mask();
  Logger::get_instance().info("login", KV("password", "secret123"), KV("user", "tom"));

  const std::string line = only_message();
  EXPECT_NE(line.find("password=***"), std::string::npos);
  EXPECT_EQ(line.find("secret123"), std::string::npos);  // 原文不得出现
  EXPECT_NE(line.find("user=tom"), std::string::npos);   // 非敏感字段不动
}

TEST_F(MaskTest, DefaultMaskerCoversAllListedKeys) {
  enable_mask();
  Logger::get_instance().info(
      "auth", KV("token", "t1"), KV("access_token", "t2"), KV("refresh_token", "t3"),
      KV("secret", "t4"), KV("authorization", "t5"), KV("cookie", "t6"), KV("private_key", "t7"),
      KV("credit_card", "t8"), KV("id_card", "t9"), KV("passwd", "t10"));

  const std::string line = only_message();
  for (const char* key : {"token", "access_token", "refresh_token", "secret", "authorization",
                          "cookie", "private_key", "credit_card", "id_card", "passwd"}) {
    EXPECT_NE(line.find(std::string(key) + "=***"), std::string::npos) << "key: " << key;
  }
}

TEST_F(MaskTest, UnmaskedNumericFieldKeepsType) {
  // 脱敏只应影响命中字段；未命中的数字字段必须保持数字类型
  LogConfig cfg;
  cfg.enable_sensitive_field_mask = true;
  cfg.format = LogFormat::JSON;
  Logger::get_instance().set_config(cfg);

  Logger::get_instance().info("order", KV("amount", 2001), KV("password", "x"));

  const std::string line = only_message();
  EXPECT_NE(line.find("\"amount\": 2001"), std::string::npos);  // 仍是数字
  EXPECT_EQ(line.find("\"amount\": \"2001\""), std::string::npos);
  EXPECT_NE(line.find("\"password\": \"***\""), std::string::npos);  // 脱敏后为字符串
}

TEST(SensitiveKeyTest, MatchesDefaultKeywordList) {
  for (const char* key : {"password", "passwd", "token", "access_token", "refresh_token", "secret",
                          "authorization", "cookie", "private_key", "credit_card", "id_card"}) {
    EXPECT_TRUE(is_sensitive_key(key)) << "key: " << key;
  }
}

TEST(SensitiveKeyTest, RejectsOthers) {
  for (const char* key : {"user_id", "amount", "request_id", "trace_id", "service", ""}) {
    EXPECT_FALSE(is_sensitive_key(key)) << "key: " << key;
  }
}

TEST_F(MaskTest, CustomMaskerReusesSensitiveKeyPredicate) {
  // 自定义 masker 复用默认关键词表：卡号前 6 后 4，其余敏感字段整体隐藏
  enable_mask([](const std::string& key, const std::string& v) -> std::string {
    if (!is_sensitive_key(key))
      return v;
    if (v.size() <= 10)
      return std::string("***");
    return v.substr(0, 6) + "****" + v.substr(v.size() - 4);
  });

  Logger::get_instance().info("pay", KV("credit_card", "6222021234567890"), KV("password", "short"),
                              KV("user_id", 42));

  const std::string line = only_message();
  EXPECT_NE(line.find("credit_card=622202****7890"), std::string::npos);
  EXPECT_NE(line.find("password=***"), std::string::npos);
  EXPECT_NE(line.find("user_id=42"), std::string::npos);  // 非敏感字段原样
}

TEST_F(MaskTest, CustomMaskerCanPreservePartOfValue) {
  enable_mask([](const std::string&, const std::string& v) {
    if (v.size() <= 4)
      return std::string("***");
    return v.substr(0, 2) + "****" + v.substr(v.size() - 2);
  });

  Logger::get_instance().info("pay", KV("card", "6222021234567890"));
  EXPECT_NE(only_message().find("card=62****90"), std::string::npos);
}

TEST_F(MaskTest, CustomMaskerSeesRawUnencodedValue) {
  // masker 拿到的是未编码的原始值：不含引号、不含转义
  std::string seen;
  enable_mask([&seen](const std::string&, const std::string& v) {
    seen = v;
    return v;
  });

  LogConfig cfg = Logger::get_instance().get_config();
  cfg.format = LogFormat::JSON;
  Logger::get_instance().set_config(cfg);

  Logger::get_instance().info("m", KV("k", "a\"b"));
  EXPECT_EQ(seen, "a\"b");  // 原始值，不是 JSON 转义后的 a\"b
}

TEST_F(MaskTest, CoversContextFields) {
  // 上下文里的敏感字段同样要脱敏
  enable_mask();
  {
    ContextScope ctx{KV("password", "ctx-secret")};
    LOG_INFO("in scope");
  }
  const std::string line = only_message();
  EXPECT_NE(line.find("password=***"), std::string::npos);
  EXPECT_EQ(line.find("ctx-secret"), std::string::npos);
}

TEST_F(MaskTest, CoversWithPreboundFields) {
  enable_mask();
  Logger::get_instance().with(KV("secret", "with-secret")).info("m");
  const std::string line = only_message();
  EXPECT_NE(line.find("secret=***"), std::string::npos);
  EXPECT_EQ(line.find("with-secret"), std::string::npos);
}

TEST_F(MaskTest, ThrowingMaskerDropsRecordWithoutLeakingOriginal) {
  // masker 抛异常时整条丢弃：绝不"保留原值继续输出"——那等于把敏感数据原样写进日志
  enable_mask([](const std::string&, const std::string&) -> std::string {
    throw std::runtime_error("masker boom");
  });

  Logger::get_instance().info("m", KV("password", "keepme"));  // 不得 terminate
  EXPECT_EQ(sink_->size(), 0u);                                // 没有写出任何东西
}

TEST_F(MaskTest, EmptyMaskerFallsBackToNoMasking) {
  LogConfig cfg;
  cfg.enable_sensitive_field_mask = true;
  cfg.sensitive_field_masker = nullptr;  // 未提供函数
  Logger::get_instance().set_config(cfg);

  Logger::get_instance().info("m", KV("password", "plain"));
  EXPECT_NE(only_message().find("password=plain"), std::string::npos);
}

TEST_F(MaskTest, MasksLastWriteWinsValueAfterDedup) {
  // 同名 key 去重后保留最后写入的值，脱敏应作用于去重之后的结果
  enable_mask();
  Logger::get_instance().info("m", KV("password", "first"), KV("password", "second"));

  const std::string line = only_message();
  EXPECT_NE(line.find("password=***"), std::string::npos);
  EXPECT_EQ(line.find("first"), std::string::npos);
  EXPECT_EQ(line.find("second"), std::string::npos);
}
