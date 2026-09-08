#include <gtest/gtest.h>
#include <memory>

#include "logger/logger.h"

#include "test_helpers.h"

// M1：Logger 核心 API —— 单例、级别过滤、多 Sink、刷新、截断
class LoggerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    Logger::get_instance().set_config(LogConfig{});
    sink_ = std::make_shared<CapturingSink>();
    Logger::get_instance().add_sink(sink_);
  }
  std::shared_ptr<CapturingSink> sink_;
};

TEST_F(LoggerTest, GetInstanceReturnsSameSingleton) {
  EXPECT_EQ(&Logger::get_instance(), &Logger::get_instance());
}

TEST_F(LoggerTest, LogsPlainStringToSink) {
  Logger::get_instance().log(LogLevel::INFO, "file.cpp", 10, "func", "hello");
  ASSERT_EQ(sink_->size(), 1u);
  const std::string line = sink_->messages().front();
  EXPECT_NE(line.find("[INFO]"), std::string::npos);
  EXPECT_NE(line.find("[file.cpp:10]"), std::string::npos);
  EXPECT_NE(line.find("hello"), std::string::npos);
}

TEST_F(LoggerTest, FormatsVariadicArguments) {
  Logger::get_instance().log(LogLevel::INFO, "f.cpp", 1, "func", "user {} login, name={}", 1001,
                             "tom");
  ASSERT_EQ(sink_->size(), 1u);
  EXPECT_NE(sink_->messages().front().find("user 1001 login, name=tom"), std::string::npos);
}

TEST_F(LoggerTest, OutputsAllFourCoreLevels) {
  // 默认级别为 TRACE，四种级别都应输出
  Logger::get_instance().log(LogLevel::DEBUG, "f.cpp", 1, "f", "debug");
  Logger::get_instance().log(LogLevel::INFO, "f.cpp", 2, "f", "info");
  Logger::get_instance().log(LogLevel::WARN, "f.cpp", 3, "f", "warn");
  Logger::get_instance().log(LogLevel::ERROR, "f.cpp", 4, "f", "error");
  ASSERT_EQ(sink_->size(), 4u);
  EXPECT_NE(sink_->messages()[0].find("[DEBUG]"), std::string::npos);
  EXPECT_NE(sink_->messages()[1].find("[INFO]"), std::string::npos);
  EXPECT_NE(sink_->messages()[2].find("[WARN]"), std::string::npos);
  EXPECT_NE(sink_->messages()[3].find("[ERROR]"), std::string::npos);
}

TEST_F(LoggerTest, LevelFilteringSuppressesLowerLevels) {
  LogConfig cfg;
  cfg.log_level = LogLevel::WARN;
  Logger::get_instance().set_config(cfg);

  Logger::get_instance().log(LogLevel::DEBUG, "f.cpp", 1, "f", "debug msg");
  Logger::get_instance().log(LogLevel::INFO, "f.cpp", 1, "f", "info msg");
  Logger::get_instance().log(LogLevel::WARN, "f.cpp", 1, "f", "warn msg");
  Logger::get_instance().log(LogLevel::ERROR, "f.cpp", 1, "f", "error msg");

  ASSERT_EQ(sink_->size(), 2u);
  EXPECT_NE(sink_->messages()[0].find("warn msg"), std::string::npos);
  EXPECT_NE(sink_->messages()[1].find("error msg"), std::string::npos);
}

TEST_F(LoggerTest, OffSuppressesEverything) {
  LogConfig cfg;
  cfg.log_level = LogLevel::OFF;
  Logger::get_instance().set_config(cfg);
  Logger::get_instance().log(LogLevel::FATAL, "f.cpp", 1, "f", "fatal");
  EXPECT_EQ(sink_->size(), 0u);
}

TEST_F(LoggerTest, DeliversToMultipleSinks) {
  auto second = std::make_shared<CapturingSink>();
  Logger::get_instance().add_sink(second);
  Logger::get_instance().log(LogLevel::INFO, "f.cpp", 1, "f", "multi");
  ASSERT_EQ(sink_->size(), 1u);
  ASSERT_EQ(second->size(), 1u);
  EXPECT_EQ(sink_->messages().front(), second->messages().front());
}

TEST_F(LoggerTest, FlushAllFlushesEverySink) {
  auto second = std::make_shared<CapturingSink>();
  Logger::get_instance().add_sink(second);
  Logger::get_instance().flush_all();
  EXPECT_EQ(sink_->flush_count(), 1);
  EXPECT_EQ(second->flush_count(), 1);
}

TEST_F(LoggerTest, TruncatesOversizedMessage) {
  LogConfig cfg;
  cfg.max_log_item_size = 32;
  Logger::get_instance().set_config(cfg);
  std::string long_msg(200, 'x');
  Logger::get_instance().log(LogLevel::INFO, "f.cpp", 1, "f", long_msg.c_str());
  ASSERT_EQ(sink_->size(), 1u);
  EXPECT_EQ(sink_->messages().front().size(), cfg.max_log_item_size);
}

TEST_F(LoggerTest, SetAndGetConfigRoundTrip) {
  LogConfig cfg;
  cfg.log_level = LogLevel::ERROR;
  cfg.max_log_item_size = 512;
  cfg.use_utc_time = true;
  Logger::get_instance().set_config(cfg);
  LogConfig got = Logger::get_instance().get_config();
  EXPECT_EQ(got.log_level, LogLevel::ERROR);
  EXPECT_EQ(got.max_log_item_size, 512u);
  EXPECT_TRUE(got.use_utc_time);
}

TEST_F(LoggerTest, MacroInterfaceWritesCompleteLines) {
  LOG_TRACE("trace {}", 1);
  LOG_DEBUG("debug");
  LOG_INFO("info {}", "x");
  LOG_WARN("warn");
  LOG_ERROR("error {}", 5);
  LOG_FATAL("fatal");
  ASSERT_EQ(sink_->size(), 6u);
  for (const auto& line : sink_->messages()) {
    EXPECT_TRUE(matches_log_line(line));
  }
}

TEST_F(LoggerTest, WithPrebindsFields) {
  auto serviceLogger = Logger::get_instance().with(KV("service", "order-service"));
  serviceLogger.info("server started");
  ASSERT_EQ(sink_->size(), 1u);
  EXPECT_NE(sink_->messages().front().find("service=order-service"), std::string::npos);
}

TEST_F(LoggerTest, StructuredFieldsReachTextSink) {
  Logger::get_instance().info("order created", KV("order_id", "ORD-1001"), KV("amount", 99.5));
  ASSERT_EQ(sink_->size(), 1u);
  const std::string line = sink_->messages().front();
  EXPECT_NE(line.find("order_id=ORD-1001"), std::string::npos);
  EXPECT_NE(line.find("amount=99.5"), std::string::npos);
}

TEST_F(LoggerTest, DedupFieldsLastWriteWins) {
  LogConfig cfg;
  cfg.format = LogFormat::JSON;
  Logger::get_instance().set_config(cfg);
  // 预绑定 service=order、env=prod；本次调用 env=dev、service=order-2 覆盖
  auto l = Logger::get_instance().with(KV("service", "order"), KV("env", "prod"));
  l.info("start", KV("env", "dev"), KV("service", "order-2"));
  ASSERT_EQ(sink_->size(), 1u);
  const std::string line = sink_->messages().front();
  EXPECT_NE(line.find("\"service\": \"order-2\""), std::string::npos);
  EXPECT_NE(line.find("\"env\": \"dev\""), std::string::npos);
  EXPECT_EQ(line.find("\"service\": \"order\""), std::string::npos);  // 旧值被覆盖
  EXPECT_EQ(line.find("\"env\": \"prod\""), std::string::npos);
}

TEST_F(LoggerTest, EmptyRuntimeKeySkipped) {
  std::string empty_key;
  Logger::get_instance().info("msg", KV(empty_key, "value"), KV("ok", 1));
  ASSERT_EQ(sink_->size(), 1u);
  const std::string line = sink_->messages().front();
  EXPECT_EQ(line.find("=value"), std::string::npos);  // 空 key 被跳过
  EXPECT_NE(line.find("ok=1"), std::string::npos);
}

TEST_F(LoggerTest, WithSkipsEmptyKey) {
  std::string empty;
  auto l = Logger::get_instance().with(KV(empty, "v"), KV("svc", "x"));
  l.info("start");
  ASSERT_EQ(sink_->size(), 1u);
  const std::string line = sink_->messages().front();
  EXPECT_EQ(line.find("=v"), std::string::npos);  // 空 key 被跳过
  EXPECT_NE(line.find("svc=x"), std::string::npos);
}

TEST_F(LoggerTest, JsonFormatSelection) {
  LogConfig cfg;
  cfg.format = LogFormat::JSON;
  Logger::get_instance().set_config(cfg);
  Logger::get_instance().log(LogLevel::INFO, "f.cpp", 1, "func", "hi");
  ASSERT_EQ(sink_->size(), 1u);
  const std::string line = sink_->messages().front();
  EXPECT_NE(line.find("\"msg\": \"hi\""), std::string::npos);
  EXPECT_NE(line.find("\"level\": \"info\""), std::string::npos);
}

TEST_F(LoggerTest, DropStrategyCountsFailure) {
  auto failing = std::make_shared<FailOnceSink>();
  Logger::get_instance().add_sink(failing);

  LogConfig cfg;
  cfg.log_fail_strategy = LogFailStrategy::Drop;
  Logger::get_instance().set_config(cfg);

  auto before = Logger::get_instance().stats();
  Logger::get_instance().info("msg");
  auto after = Logger::get_instance().stats();
  EXPECT_EQ(after.failed_writes - before.failed_writes, 1u);
  EXPECT_EQ(after.dropped - before.dropped, 1u);
}

TEST_F(LoggerTest, FallbackStrategyCountsButNotDrop) {
  auto failing = std::make_shared<FailOnceSink>();
  Logger::get_instance().add_sink(failing);

  LogConfig cfg;
  cfg.log_fail_strategy = LogFailStrategy::FallbackToStderr;
  Logger::get_instance().set_config(cfg);

  auto before = Logger::get_instance().stats();
  Logger::get_instance().info("msg");  // 会额外写一条到 stderr
  auto after = Logger::get_instance().stats();
  EXPECT_EQ(after.failed_writes - before.failed_writes, 1u);
  EXPECT_EQ(after.dropped - before.dropped, 0u);
}

TEST_F(LoggerTest, ThrowingSinkDoesNotCrash) {
  auto throwing = std::make_shared<ThrowOnceSink>();
  Logger::get_instance().add_sink(throwing);

  auto before = Logger::get_instance().stats();
  Logger::get_instance().info("msg");  // 若没 catch 会 terminate
  auto after = Logger::get_instance().stats();
  EXPECT_EQ(after.failed_writes - before.failed_writes, 1u);
}

TEST(FilenameOfTest, StripsDirectoryPrefix) {
  EXPECT_STREQ(filename_of("/a/b/c/file.cpp"), "file.cpp");
  EXPECT_STREQ(filename_of("file.cpp"), "file.cpp");
  EXPECT_STREQ(filename_of("a\\b\\c\\file.cpp"), "file.cpp");
}
