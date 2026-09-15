#include <chrono>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <thread>

#include "logger/detail/dedup.h"
#include "logger/logger.h"

#include "test_helpers.h"

using namespace logger;          // 库的公共符号
using namespace logger::detail;  // 白盒用例要直接构造 Record / Formatter 等内部类型

// M6：聚合去重 —— 相同 (level, file, line) 在窗口内只输出首条，序列结束补重复次数

// ===== DedupFilter 纯逻辑 =====

TEST(DedupFilterTest, DisabledWindowAlwaysEmits) {
  DedupFilter f;
  for (int i = 0; i < 5; ++i) {
    std::uint64_t prev = 0;
    bool need = false;
    EXPECT_EQ(f.on_log(LogLevel::INFO, "a.cpp", 1, /*window_ms=*/0, &prev, &need),
              DedupFilter::Decision::Emit);
    EXPECT_EQ(prev, 0u);
    EXPECT_FALSE(need);
  }
}

TEST(DedupFilterTest, SuppressesSameKeyWithinWindow) {
  DedupFilter f;
  std::uint64_t prev = 0;
  bool need = false;

  EXPECT_EQ(f.on_log(LogLevel::INFO, "a.cpp", 1, 1000, &prev, &need),
            DedupFilter::Decision::Emit);  // 首条

  EXPECT_EQ(f.on_log(LogLevel::INFO, "a.cpp", 1, 1000, &prev, &need),
            DedupFilter::Decision::Suppress);
  EXPECT_TRUE(need);  // 首次重复：需要调用方提供载荷
  EXPECT_EQ(f.count(), 2u);

  EXPECT_EQ(f.on_log(LogLevel::INFO, "a.cpp", 1, 1000, &prev, &need),
            DedupFilter::Decision::Suppress);
  EXPECT_FALSE(need);  // 后续重复不再需要
  EXPECT_EQ(f.count(), 3u);
}

TEST(DedupFilterTest, DifferentKeySwitchesSeries) {
  DedupFilter f;
  std::uint64_t prev = 0;
  bool need = false;
  f.on_log(LogLevel::INFO, "a.cpp", 1, 1000, &prev, &need);  // 序列 A
  f.on_log(LogLevel::INFO, "a.cpp", 1, 1000, &prev, &need);

  EXPECT_EQ(f.on_log(LogLevel::INFO, "a.cpp", 2, 1000, &prev, &need),  // 不同行 → 新序列
            DedupFilter::Decision::Emit);
  EXPECT_EQ(prev, 2u);  // 上一条序列的次数交出来
  EXPECT_EQ(f.count(), 1u);
}

TEST(DedupFilterTest, LevelParticipatesInKey) {
  DedupFilter f;
  std::uint64_t prev = 0;
  bool need = false;
  f.on_log(LogLevel::INFO, "a.cpp", 1, 1000, &prev, &need);
  EXPECT_EQ(f.on_log(LogLevel::ERROR, "a.cpp", 1, 1000, &prev, &need),  // 级别不同 → 新序列
            DedupFilter::Decision::Emit);
  EXPECT_EQ(prev, 1u);
}

TEST(DedupFilterTest, WindowExpiryStartsNewSeries) {
  DedupFilter f;
  std::uint64_t prev = 0;
  bool need = false;
  f.on_log(LogLevel::INFO, "a.cpp", 1, /*window_ms=*/1, &prev, &need);
  f.on_log(LogLevel::INFO, "a.cpp", 1, 1, &prev, &need);

  std::this_thread::sleep_for(std::chrono::milliseconds(5));  // 等窗口过期

  EXPECT_EQ(f.on_log(LogLevel::INFO, "a.cpp", 1, 1, &prev, &need), DedupFilter::Decision::Emit);
  EXPECT_EQ(prev, 2u);
}

TEST(DedupFilterTest, TakePrevCountResets) {
  DedupFilter f;
  std::uint64_t prev = 0;
  bool need = false;
  f.on_log(LogLevel::INFO, "a.cpp", 1, 1000, &prev, &need);
  f.on_log(LogLevel::INFO, "a.cpp", 1, 1000, &prev, &need);

  EXPECT_EQ(f.take_prev_count(), 2u);
  EXPECT_EQ(f.take_prev_count(), 0u);  // 已复位
  EXPECT_FALSE(f.active());
}

// ===== 与 Logger 的接线 =====

class DedupLogTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dedup_filter().take_prev_count();  // 清掉上一个用例遗留的 thread_local 状态
    Logger::get_instance().set_config(LogConfig{});
    sink_ = std::make_shared<CapturingSink>();
    Logger::get_instance().add_sink(sink_);
  }

  void enable(std::size_t window_ms = 1000) {
    LogConfig cfg;
    cfg.dedup_window_ms = window_ms;
    Logger::get_instance().set_config(cfg);
  }

  std::shared_ptr<CapturingSink> sink_;
};

TEST_F(DedupLogTest, DisabledByDefaultEmitsEveryLine) {
  for (int i = 0; i < 5; ++i)
    Logger::get_instance().log(LogLevel::ERROR, "site.cpp", 42, "f", "boom");
  EXPECT_EQ(sink_->size(), 5u);
}

TEST_F(DedupLogTest, CollapsesRepeatedLogsIntoFirstPlusSummary) {
  enable();
  for (int i = 0; i < 100; ++i)
    Logger::get_instance().log(LogLevel::ERROR, "site.cpp", 42, "f", "boom");
  Logger::get_instance().flush_all();

  ASSERT_EQ(sink_->size(), 2u);  // 首条 + 摘要
  EXPECT_NE(sink_->messages()[0].find("boom"), std::string::npos);
  EXPECT_EQ(sink_->messages()[0].find("repeated"), std::string::npos);  // 首条不带计数
  EXPECT_NE(sink_->messages()[1].find("(repeated 100 times)"), std::string::npos);
}

TEST_F(DedupLogTest, SummaryKeepsFieldsAndContext) {
  enable();
  {
    ContextScope ctx{KV("request_id", "r1")};
    for (int i = 0; i < 3; ++i)
      Logger::get_instance().info("db failed", KV("err", "timeout"));
  }
  Logger::get_instance().flush_all();

  ASSERT_EQ(sink_->size(), 2u);
  const std::string summary = sink_->messages()[1];
  EXPECT_NE(summary.find("(repeated 3 times)"), std::string::npos);
  EXPECT_NE(summary.find("err=timeout"), std::string::npos);    // 字段保留
  EXPECT_NE(summary.find("request_id=r1"), std::string::npos);  // 上下文保留
}

TEST_F(DedupLogTest, DifferentSitesAreNotMerged) {
  enable();
  Logger::get_instance().log(LogLevel::ERROR, "a.cpp", 1, "f", "A");
  Logger::get_instance().log(LogLevel::ERROR, "b.cpp", 1, "f", "B");   // 不同文件
  Logger::get_instance().log(LogLevel::ERROR, "a.cpp", 2, "f", "A2");  // 同一文件不同行
  EXPECT_EQ(sink_->size(), 3u);
}

TEST_F(DedupLogTest, JsonSummaryCarriesNumericMember) {
  LogConfig cfg;
  cfg.dedup_window_ms = 1000;
  cfg.format = LogFormat::JSON;
  Logger::get_instance().set_config(cfg);

  for (int i = 0; i < 7; ++i)
    Logger::get_instance().log(LogLevel::ERROR, "site.cpp", 42, "f", "boom");
  Logger::get_instance().flush_all();

  ASSERT_EQ(sink_->size(), 2u);
  EXPECT_NE(sink_->messages()[1].find("\"repeated\": 7"), std::string::npos);  // 数字成员
}

TEST_F(DedupLogTest, FlushAllEmitPendingSummary) {
  enable();
  for (int i = 0; i < 4; ++i)
    Logger::get_instance().log(LogLevel::ERROR, "site.cpp", 42, "f", "boom");
  EXPECT_EQ(sink_->size(), 1u);  // 还没收尾

  Logger::get_instance().flush_all();
  ASSERT_EQ(sink_->size(), 2u);
  EXPECT_NE(sink_->messages()[1].find("(repeated 4 times)"), std::string::npos);
}

TEST_F(DedupLogTest, SingleOccurrenceProducesNoSummary) {
  enable();
  Logger::get_instance().log(LogLevel::ERROR, "site.cpp", 42, "f", "boom");
  Logger::get_instance().flush_all();  // 没重复过，不该补摘要
  EXPECT_EQ(sink_->size(), 1u);
}
