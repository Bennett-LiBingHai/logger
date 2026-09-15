#include <chrono>
#include <filesystem>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

#include "logger/logger.h"

using namespace logger;          // 库的公共符号
using namespace logger::detail;  // 白盒用例要直接构造 Record / Formatter 等内部类型

// M7：生命周期 —— 并发 close、重复 close、线程回收
//
// 独立二进制：单例的异步线程一旦 close 便不可重启，会污染其它用例。
// 注：单例的析构同样调用 close()，故这里对 close() 的覆盖也覆盖析构路径。

namespace {

// 当前进程的线程数（读 /proc/self/task 的条目数）
size_t thread_count() {
  std::error_code ec;
  size_t n = 0;
  for (auto it = std::filesystem::directory_iterator("/proc/self/task", ec);
       it != std::filesystem::directory_iterator(); it.increment(ec)) {
    if (ec)
      break;
    ++n;
  }
  return n;
}

}  // namespace

TEST(LifecycleTest, ConcurrentCloseIsSafeAndDoesNotLeakThreads) {
  LogConfig cfg;
  cfg.async = true;
  Logger::get_instance().set_config(cfg);

  for (int i = 0; i < 200; ++i)
    Logger::get_instance().info("msg {}", i);

  const size_t with_async = thread_count();

  // 多线程同时 close：不得崩溃、不得挂住
  constexpr int kThreads = 8;
  std::vector<std::thread> closers;
  closers.reserve(kThreads);
  for (int i = 0; i < kThreads; ++i)
    closers.emplace_back([] { Logger::get_instance().close(); });
  for (auto& t : closers)
    t.join();

  EXPECT_LE(thread_count(), with_async - 1) << "后台线程应已回收";
}

TEST(LifecycleTest, RepeatedCloseIsIdempotent) {
  const LogStats first = Logger::get_instance().close();
  const LogStats second = Logger::get_instance().close();  // 再关一次不应有问题
  EXPECT_EQ(first.written, second.written);
  EXPECT_EQ(first.dropped, second.dropped);
}

// close() 之后队列不再有消费者，再打日志必须被丢弃而不是入队。
// Block 策略下尤其危险：入队会等一个永远不会到来的空间，直接把调用方卡死
TEST(LifecycleTest, LogAfterCloseIsDroppedAndDoesNotBlock) {
  LogConfig cfg;
  cfg.async = true;
  cfg.buffer_size = 2;
  cfg.asy_que_ful_strategy = AsyQueFulStrategy::Block;
  Logger::get_instance().set_config(cfg);

  const LogStats before = Logger::get_instance().close();

  for (int i = 0; i < 100; ++i)
    Logger::get_instance().info("after close {}", i);  // 满队列 + Block，入队即卡死

  const LogStats after = Logger::get_instance().stats();
  EXPECT_EQ(after.dropped - before.dropped, 100u);
  EXPECT_EQ(after.written, before.written);
}

// 注：close() 在本二进制里是终态的（异步线程不可重启），
// 「close 排空队列」的语义由 test_async_close 覆盖。
