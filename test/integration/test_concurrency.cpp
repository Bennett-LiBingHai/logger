#include <gtest/gtest.h>
#include <memory>
#include <set>
#include <thread>
#include <vector>

#include "logger/logger.h"

#include "test_helpers.h"

// M1：并发安全 —— 多线程同时写日志，无数据竞争、内容不交叉、不丢失。
// 本测试也可在 ThreadSanitizer 下运行（编译加 -fsanitize=thread）验证无数据竞争。
class LoggerConcurrencyTest : public ::testing::Test {
 protected:
  void SetUp() override {
    Logger::get_instance().set_config(LogConfig{});
    sink_ = std::make_shared<CapturingSink>();
    Logger::get_instance().add_sink(sink_);
  }
  std::shared_ptr<CapturingSink> sink_;
};

TEST_F(LoggerConcurrencyTest, ConcurrentLoggingIsRaceFreeAndLossless) {
  constexpr int kThreads = 8;
  constexpr int kPerThread = 500;

  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([t]() {
      for (int m = 0; m < kPerThread; ++m) {
        LOG_INFO("thread_%d_msg_%d", t, m);
      }
    });
  }
  for (auto& th : threads)
    th.join();

  const auto msgs = sink_->messages();
  // 1. 数量正确：无丢失、无重复
  ASSERT_EQ(msgs.size(), static_cast<size_t>(kThreads * kPerThread));
  // 2. 每条日志都是完整的一行：无交叉/覆盖
  for (const auto& line : msgs) {
    EXPECT_TRUE(matches_log_line(line));
  }
  // 3. 内容唯一：每条 (thread, msg) 恰好出现一次
  std::set<std::string> unique(msgs.begin(), msgs.end());
  EXPECT_EQ(unique.size(), msgs.size());
}
