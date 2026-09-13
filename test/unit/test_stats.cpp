#include <gtest/gtest.h>
#include <memory>
#include <ostream>
#include <stdexcept>
#include <string>

#include "logger/logger.h"

#include "test_helpers.h"

// M6：日志自身指标 —— 写入/失败/丢弃/按级别/编码失败/脱敏/聚合去重
//
// 单例计数跨用例累积，故断言一律取基线差值。

namespace {

// operator<< 抛异常的类型：用于触发编码失败
struct ThrowingStreamable {};
std::ostream& operator<<(std::ostream& os, const ThrowingStreamable&) {
  throw std::runtime_error("operator<< boom");
}

}  // namespace

class StatsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    Logger::get_instance().set_config(LogConfig{});
    sink_ = std::make_shared<CapturingSink>();
    Logger::get_instance().add_sink(sink_);
    base_ = Logger::get_instance().stats();
  }

  std::shared_ptr<CapturingSink> sink_;
  LogStats base_;
};

TEST_F(StatsTest, CountsWrittenAndByLevel) {
  Logger::get_instance().info("a");
  Logger::get_instance().warn("b");
  Logger::get_instance().error("c");
  Logger::get_instance().error("d");

  const LogStats s = Logger::get_instance().stats();
  EXPECT_EQ(s.written - base_.written, 4u);
  EXPECT_EQ(s.by_level[static_cast<std::size_t>(LogLevel::INFO)] -
                base_.by_level[static_cast<std::size_t>(LogLevel::INFO)],
            1u);
  EXPECT_EQ(s.by_level[static_cast<std::size_t>(LogLevel::WARN)] -
                base_.by_level[static_cast<std::size_t>(LogLevel::WARN)],
            1u);
  EXPECT_EQ(s.by_level[static_cast<std::size_t>(LogLevel::ERROR)] -
                base_.by_level[static_cast<std::size_t>(LogLevel::ERROR)],
            2u);
}

TEST_F(StatsTest, CountsFailedWritesAndDropped) {
  auto failing = std::make_shared<FailOnceSink>();
  Logger::get_instance().add_sink(failing);

  LogConfig cfg;
  cfg.log_fail_strategy = LogFailStrategy::Drop;
  Logger::get_instance().set_config(cfg);

  Logger::get_instance().info("boom");

  const LogStats s = Logger::get_instance().stats();
  EXPECT_EQ(s.failed_writes - base_.failed_writes, 1u);
  EXPECT_EQ(s.dropped - base_.dropped, 1u);
  EXPECT_EQ(s.written - base_.written, 1u);  // 另一个 sink 写成功了
}

TEST_F(StatsTest, CountsDroppedOnFieldEncodeFailure) {
  // 字段编码抛异常：整条丢弃并计数，不写出、不崩
  Logger::get_instance().info("m", KV("bad", ThrowingStreamable{}));

  const LogStats s = Logger::get_instance().stats();
  EXPECT_EQ(s.dropped - base_.dropped, 1u);
  EXPECT_EQ(s.written, base_.written);  // 这条没写出去
}

TEST_F(StatsTest, PositionalArgThrowingOperatorIsDroppedNotFatal) {
  // 位置参数在 log_impl 里编码；抛异常时该条整体丢弃并计数，绝不能 terminate
  Logger::get_instance().info("v={}", ThrowingStreamable{});

  const LogStats s = Logger::get_instance().stats();
  EXPECT_EQ(s.dropped - base_.dropped, 1u);
  EXPECT_EQ(s.written, base_.written);  // 没写出去
}

TEST_F(StatsTest, ThrowingMaskerDropsWholeRecord) {
  // masker 抛异常：整条丢弃，绝不"保留原值继续输出"——那等于把敏感数据原样写进日志
  LogConfig cfg;
  cfg.enable_sensitive_field_mask = true;
  cfg.sensitive_field_masker = [](const std::string&, const std::string&) -> std::string {
    throw std::runtime_error("masker boom");
  };
  Logger::get_instance().set_config(cfg);

  Logger::get_instance().info("m", KV("password", "secret"));

  const LogStats s = Logger::get_instance().stats();
  EXPECT_EQ(s.dropped - base_.dropped, 1u);
  EXPECT_EQ(sink_->size(), 0u);  // 原文没有泄出去
}

TEST_F(StatsTest, MaskedFieldsDoNotBreakNormalEncoding) {
  // 脱敏与位置参数编码互不影响（回归：确认上面的 try/catch 没吞掉正常路径）
  Logger::get_instance().info("m={}", 42);
  EXPECT_EQ(sink_->size(), 1u);
}

TEST_F(StatsTest, CountsMaskedFields) {
  LogConfig cfg;
  cfg.enable_sensitive_field_mask = true;
  Logger::get_instance().set_config(cfg);

  Logger::get_instance().info("m", KV("password", "x"), KV("token", "y"), KV("user", "tom"));

  const LogStats s = Logger::get_instance().stats();
  EXPECT_EQ(s.masked_fields - base_.masked_fields, 2u);  // 命中两个敏感字段
}

TEST_F(StatsTest, CountsDedupSuppressed) {
  dedup_filter().take_prev_count();  // 清掉遗留的 thread_local 状态

  LogConfig cfg;
  cfg.dedup_window_ms = 1000;
  Logger::get_instance().set_config(cfg);

  for (int i = 0; i < 10; ++i)
    Logger::get_instance().log(LogLevel::ERROR, "site.cpp", 7, "f", "boom");
  Logger::get_instance().flush_all();

  const LogStats s = Logger::get_instance().stats();
  EXPECT_EQ(s.dedup_suppressed - base_.dedup_suppressed, 9u);  // 10 条里 9 条被抑制
}

TEST_F(StatsTest, QueueMetricsAreZeroInSyncMode) {
  Logger::get_instance().info("m");
  const LogStats s = Logger::get_instance().stats();
  EXPECT_EQ(s.queue_length, 0u);
  EXPECT_EQ(s.queue_peak, 0u);
}

TEST_F(StatsTest, StatsAreReadableWithoutBlockingWrites) {
  // stats() 只对队列长度短暂持锁，计数走原子；反复读取不应影响写入
  for (int i = 0; i < 100; ++i) {
    Logger::get_instance().info("m {}", i);
    (void)Logger::get_instance().stats();
  }
  EXPECT_EQ(sink_->size(), 100u);
}
