#include <cstdio>
#include <logger/logger.h>
#include <logger/sink/file_sink.h>
#include <memory>
#include <thread>
#include <vector>

// 异步写入：业务线程只入队，后台线程负责格式化与落盘
int main() {
  LogConfig cfg;
  cfg.async = true;
  cfg.buffer_size = 10000;
  cfg.asy_que_ful_strategy = AsyQueFulStrategy::Block;  // 队列满时阻塞，不丢日志
  Logger::get_instance().set_config(cfg);               // 惰性启动后台线程

  FileSinkConfig sink_cfg;
  sink_cfg.dir = "/tmp/logger_example_async";
  Logger::get_instance().add_sink(std::make_shared<FileSink>(sink_cfg));

  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t)
    threads.emplace_back([t] {
      for (int i = 0; i < 1000; ++i)
        LOG_INFO("thread {} seq {}", t, i);
    });
  for (auto& th : threads)
    th.join();

  Logger::get_instance().flush_all();  // 等队列清空并写完

  // 汇总打到终端（不挂 ConsoleSink，避免 4000 条明细刷屏）
  const LogStats s = Logger::get_instance().stats();
  std::printf("written=%llu dropped=%llu queue_peak=%llu max_latency_us=%llu\n", s.written,
              s.dropped, s.queue_peak, s.max_write_latency_us);
  std::printf("明细见 /tmp/logger_example_async/\n");
  return 0;
}
