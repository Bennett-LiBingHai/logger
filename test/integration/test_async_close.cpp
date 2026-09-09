#include <gtest/gtest.h>
#include <memory>

#include "logger/logger.h"

#include "test_helpers.h"

// 优雅关闭：独立二进制（close() 会停止异步线程，单例不可复用）。
// 验证 close() 能把队列中剩余日志完整刷出，且返回统计信息。
TEST(AsyncCloseTest, CloseDrainsQueue) {
  auto sink = std::make_shared<CapturingSink>();
  Logger::get_instance().add_sink(sink);

  LogConfig cfg;
  cfg.async = true;
  cfg.buffer_size = 65536;
  Logger::get_instance().set_config(cfg);

  const int k = 100000;
  for (int i = 0; i < k; ++i)
    Logger::get_instance().info("msg {}", i);

  // 不显式 flush，直接 close()：应等待后台线程消费完队列并写完
  LogStats stats = Logger::get_instance().close();

  EXPECT_EQ(sink->size(), static_cast<size_t>(k));
  // close() 返回的统计应为 0（无写失败、无丢弃）
  EXPECT_EQ(stats.failed_writes, 0u);
  EXPECT_EQ(stats.dropped, 0u);
}
