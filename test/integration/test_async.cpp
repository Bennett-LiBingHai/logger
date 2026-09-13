#include <atomic>
#include <gtest/gtest.h>
#include <memory>
#include <ostream>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

#include "logger/logger.h"

#include "test_helpers.h"

// 异步日志：独立二进制，避免污染 test_logger 的单例状态。
// 单例的异步线程一旦启动便不可回退，故本文件内的用例共享同一个异步 Logger，
// 每个用例结束前都会 flush_all() 排空队列，避免相互影响。

namespace {

// operator<< 抛异常的类型：用于让后台写入线程在格式化时抛
struct ThrowingStreamable {};
std::ostream& operator<<(std::ostream& os, const ThrowingStreamable&) {
  throw std::runtime_error("operator<< boom");
}

// 配置异步模式并返回新捕获 Sink（每个用例用独立 sink，断言只针对该 sink）
std::shared_ptr<CapturingSink> set_async(LogConfig cfg) {
  cfg.async = true;
  Logger::get_instance().set_config(cfg);
  auto sink = std::make_shared<CapturingSink>();
  Logger::get_instance().add_sink(sink);
  return sink;
}

}  // namespace

// 后台写入抛异常：该条丢弃、后台线程存活、flush_all 不挂住
TEST(AsyncTest, ThrowingFieldInWriterIsDroppedAndLoopSurvives) {
  auto sink = std::make_shared<CapturingSink>();
  Logger::get_instance().add_sink(sink);

  LogConfig cfg;
  cfg.async = true;
  Logger::get_instance().set_config(cfg);

  const auto before = Logger::get_instance().stats().dropped;
  Logger::get_instance().info("bad", KV("k", ThrowingStreamable{}));
  Logger::get_instance().info("good");  // 后台线程必须还活着
  Logger::get_instance().flush_all();   // 且不得永久等待

  const LogStats after = Logger::get_instance().stats();
  EXPECT_EQ(after.dropped - before, 1u);
  EXPECT_EQ(after.queue_length, 0u);
  ASSERT_EQ(sink->size(), 1u);  // 只有 good 写出来了
  EXPECT_NE(sink->messages().front().find("good"), std::string::npos);
}

// 自身指标：用 GateSink 卡住后台线程，让队列堆积，检查长度与峰值
TEST(AsyncTest, TracksQueueLengthAndPeak) {
  LogConfig cfg;
  cfg.async = true;
  cfg.buffer_size = 1000;  // 远大于本次压入数量，避免触发队列满策略
  Logger::get_instance().set_config(cfg);

  auto gate = std::make_shared<GateSink>(1);  // 第一次 log 即阻塞后台线程
  Logger::get_instance().add_sink(gate);

  for (int i = 0; i < 50; ++i)
    Logger::get_instance().info("msg {}", i);

  gate->wait_received(1);  // 后台线程已取走第一条并卡住

  const LogStats s = Logger::get_instance().stats();
  EXPECT_GT(s.queue_length, 0u);  // 剩下的都还在队列里
  EXPECT_GE(s.queue_peak, s.queue_length);
  EXPECT_GE(s.queue_peak, 40u);  // 50 条减去被取走的那条左右
  // 注意：此刻后台线程正卡在 sink 里，还没有任何一条完成写出，written 应为 0

  gate->open();
  Logger::get_instance().flush_all();
  const LogStats after = Logger::get_instance().stats();
  EXPECT_EQ(after.queue_length, 0u);  // 已排空
  EXPECT_GE(after.written, 50u);  // 至少这 50 条（同二进制内其它用例可能另有计数）
}

// 基本：异步模式下所有消息不丢（flush 后队列清空并写完）
TEST(AsyncTest, DeliversAllMessages) {
  auto sink = std::make_shared<CapturingSink>();
  Logger::get_instance().add_sink(sink);

  LogConfig cfg;
  cfg.async = true;
  Logger::get_instance().set_config(cfg);

  const int k = 1000;
  for (int i = 0; i < k; ++i)
    Logger::get_instance().info("msg {}", i);

  // 等队列清空并写完（in_flight 归零）
  Logger::get_instance().flush_all();
  EXPECT_EQ(sink->size(), static_cast<size_t>(k));
}

// 异步 + 并发：多线程同时异步写，无丢失、无重复、无交叉
TEST(AsyncTest, ConcurrentAsyncIsLossless) {
  LogConfig cfg;
  cfg.async = true;
  cfg.buffer_size = 65536;
  cfg.asy_que_ful_strategy = AsyQueFulStrategy::Block;
  auto sink = set_async(cfg);

  constexpr int kThreads = 8;
  constexpr int kPerThread = 500;

  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([t]() {
      for (int m = 0; m < kPerThread; ++m)
        LOG_INFO("t{}_m{}", t, m);
    });
  }
  for (auto& th : threads)
    th.join();

  Logger::get_instance().flush_all();
  const auto msgs = sink->messages();
  ASSERT_EQ(msgs.size(), static_cast<size_t>(kThreads * kPerThread));
  for (const auto& line : msgs)
    EXPECT_TRUE(matches_log_line(line));
  std::set<std::string> unique(msgs.begin(), msgs.end());
  EXPECT_EQ(unique.size(), msgs.size());
}

// 队列满策略：Block —— 队列满时阻塞生产者，直到后台线程腾出空间，最终不丢
TEST(AsyncQueueFullTest, BlockWaitsForSpace) {
  constexpr size_t kBuf = 4;
  auto sink = std::make_shared<GateSink>();
  Logger::get_instance().add_sink(sink);

  LogConfig cfg;
  cfg.async = true;
  cfg.buffer_size = kBuf;
  cfg.asy_que_ful_strategy = AsyQueFulStrategy::Block;
  Logger::get_instance().set_config(cfg);

  // 1) 首条消息：后台线程取走并阻塞在 sink（卡住消费者）
  Logger::get_instance().info("seed");
  sink->wait_received(1);

  // 2) 填满队列（consumer 阻塞，队列可容纳 kBuf 条）
  for (size_t i = 0; i < kBuf; ++i)
    Logger::get_instance().info("fill {}", i);

  // 3) 第 kBuf+1 条：队列已满 → Block，生产者应被阻塞
  std::atomic<bool> done{false};
  std::thread producer([&] {
    Logger::get_instance().info("blocked");
    done = true;
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_FALSE(done.load()) << "队列满时 Block 策略应阻塞生产者";

  sink->open();  // 放行消费者
  producer.join();
  EXPECT_TRUE(done.load());

  Logger::get_instance().flush_all();
  EXPECT_EQ(sink->size(), 1u + kBuf + 1u);  // seed + fills + blocked
}

// 队列满策略：DropNewest —— 队列满时丢弃新日志
TEST(AsyncQueueFullTest, DropNewestDropsNewMessages) {
  constexpr size_t kBuf = 4;
  auto sink = std::make_shared<GateSink>();
  Logger::get_instance().add_sink(sink);

  LogConfig cfg;
  cfg.async = true;
  cfg.buffer_size = kBuf;
  cfg.asy_que_ful_strategy = AsyQueFulStrategy::DropNewest;
  Logger::get_instance().set_config(cfg);

  Logger::get_instance().info("seed");
  sink->wait_received(1);

  for (size_t i = 0; i < kBuf; ++i)
    Logger::get_instance().info("fill {}", i);

  // 队列已满，这些新日志应被丢弃
  for (int i = 0; i < 3; ++i)
    Logger::get_instance().info("dropped {}", i);

  sink->open();
  Logger::get_instance().flush_all();

  ASSERT_EQ(sink->size(), 1u + kBuf);  // seed + fills
  EXPECT_FALSE(sink->contains("dropped"));
  for (size_t i = 0; i < kBuf; ++i)
    EXPECT_TRUE(sink->contains("fill " + std::to_string(i)));
}

// 队列满策略：DropOldest —— 队列满时丢弃最旧日志，保留新日志
TEST(AsyncQueueFullTest, DropOldestDropsOldMessages) {
  constexpr size_t kBuf = 4;
  auto sink = std::make_shared<GateSink>();
  Logger::get_instance().add_sink(sink);

  LogConfig cfg;
  cfg.async = true;
  cfg.buffer_size = kBuf;
  cfg.asy_que_ful_strategy = AsyQueFulStrategy::DropOldest;
  Logger::get_instance().set_config(cfg);

  Logger::get_instance().info("seed");
  sink->wait_received(1);

  for (size_t i = 0; i < kBuf; ++i)
    Logger::get_instance().info("fill {}", i);

  Logger::get_instance().info("newest");  // 满 → 丢最旧的 fill 0

  sink->open();
  Logger::get_instance().flush_all();

  ASSERT_EQ(sink->size(), 1u + kBuf);  // seed + (fill1..fill3 + newest)
  EXPECT_TRUE(sink->contains("newest"));
  EXPECT_FALSE(sink->contains("fill 0"));
  EXPECT_TRUE(sink->contains("fill 1"));
}

// 队列满策略：DropDebug —— 队列满时优先丢弃低级别日志，保留高优先级新日志
TEST(AsyncQueueFullTest, DropDebugPrefersDroppingLowLevel) {
  constexpr size_t kBuf = 2;
  auto sink = std::make_shared<GateSink>();
  Logger::get_instance().add_sink(sink);

  LogConfig cfg;
  cfg.async = true;
  cfg.buffer_size = kBuf;
  cfg.asy_que_ful_strategy = AsyQueFulStrategy::DropDebug;
  Logger::get_instance().set_config(cfg);

  Logger::get_instance().info("seed");
  sink->wait_received(1);

  // 用两条 INFO 填满队列
  Logger::get_instance().info("info0");
  Logger::get_instance().info("info1");

  // 满：ERROR / WARN 应各自顶掉一条 INFO
  Logger::get_instance().error("err0");
  Logger::get_instance().warn("warn0");

  sink->open();
  Logger::get_instance().flush_all();

  ASSERT_EQ(sink->size(), 1u + 2u);  // seed + err0 + warn0
  EXPECT_TRUE(sink->contains("err0"));
  EXPECT_TRUE(sink->contains("warn0"));
  EXPECT_FALSE(sink->contains("info0"));
  EXPECT_FALSE(sink->contains("info1"));
}
