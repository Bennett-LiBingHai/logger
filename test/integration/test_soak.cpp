#include <atomic>
#include <chrono>
#include <cstdlib>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "logger/logger.h"

#include "test_helpers.h"

using namespace logger;          // 库的公共符号
using namespace logger::detail;  // 白盒用例要直接构造 Record / Formatter 等内部类型

// M7：持续高并发 —— 固定时长内多线程持续写入，断言不丢、不交叉、不崩
//
// 时长默认 2 秒（CI 友好），可用环境变量调大：
//   LOGGER_SOAK_SECONDS=30 ./build/test_soak

namespace {

constexpr int kThreads = 8;
constexpr size_t kSampleLines = 200;  // 只留少量样本做格式校验，避免内存爆掉

// 只计数 + 留少量样本的 Sink：压测时不能把每条都存下来
class CountingSink : public LogSink {
 public:
  bool log(const SinkInput& result) override {
    if (sample_.size() < kSampleLines) {
      std::lock_guard<std::mutex> lock(mtx_);
      if (sample_.size() < kSampleLines)
        sample_.push_back(result.formatted_msg);
    }
    ++count_;
    return true;
  }
  void flush() override {}

  [[nodiscard]] unsigned long long count() const {
    return count_.load();
  }
  std::vector<std::string> sample() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return sample_;
  }

 private:
  std::atomic<unsigned long long> count_{0};
  mutable std::mutex mtx_;
  mutable std::vector<std::string> sample_;
};

std::chrono::milliseconds soak_duration() {
  const char* env = std::getenv("LOGGER_SOAK_SECONDS");
  const long secs = (env != nullptr) ? std::strtol(env, nullptr, 10) : 2;
  return std::chrono::milliseconds((secs > 0 ? secs : 1) * 1000);
}

// 持续写满指定时长，返回写出的条数
unsigned long long run_load(int id, std::chrono::milliseconds duration) {
  const auto deadline = std::chrono::steady_clock::now() + duration;
  unsigned long long n = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    LOG_INFO("t{} n{}", id, n);
    ++n;
  }
  return n;
}

unsigned long long run_all(const std::shared_ptr<LogSink>& sink_unused) {
  (void)sink_unused;
  const auto duration = soak_duration();
  std::vector<unsigned long long> sent(kThreads, 0);
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int i = 0; i < kThreads; ++i)
    threads.emplace_back([&, i] { sent[i] = run_load(i, duration); });
  for (auto& t : threads)
    t.join();

  unsigned long long total = 0;
  for (const auto v : sent)
    total += v;
  return total;
}

}  // namespace

// 同步模式：每条都落盘，计数必须完全相等
TEST(SoakTest, SustainedConcurrentSyncIsLossless) {
  auto sink = std::make_shared<CountingSink>();
  Logger::get_instance().add_sink(sink);
  Logger::get_instance().set_config(LogConfig{});

  const unsigned long long total = run_all(sink);

  ASSERT_GT(total, 0ull);
  EXPECT_EQ(sink->count(), total) << "同步模式不应丢日志";

  for (const auto& line : sink->sample())
    EXPECT_TRUE(matches_log_line(line)) << "行结构被破坏: " << line;
}

// 异步模式 + Block 策略：close 前必须一条不丢、且不卡死
TEST(SoakTest, SustainedConcurrentAsyncIsLossless) {
  auto sink = std::make_shared<CountingSink>();
  Logger::get_instance().add_sink(sink);

  LogConfig cfg;
  cfg.async = true;
  cfg.buffer_size = 65536;
  cfg.asy_que_ful_strategy = AsyQueFulStrategy::Block;
  Logger::get_instance().set_config(cfg);

  const unsigned long long base = sink->count();
  const unsigned long long total = run_all(sink);

  Logger::get_instance().flush_all();  // 任一用例挂死在这里即为失败（超时）
  ASSERT_GT(total, 0ull);
  EXPECT_EQ(sink->count() - base, total) << "异步 Block 策略不应丢日志";

  for (const auto& line : sink->sample())
    EXPECT_TRUE(matches_log_line(line)) << "行结构被破坏: " << line;
}
