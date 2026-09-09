// 性能基准：自研轻量 benchmark（零外部依赖），输出吞吐与 P50/P95/P99 延迟。
//
// 对照 docs/milestone.md 的 M4 性能目标：
//   关闭级别日志开销尽量低 / 同步文本 ≥ 100K 条/秒 / 异步文本 ≥ 300K 条/秒
//   关键路径额外延迟 P99 < 1ms
//
// 注意：Logger 是单例，异步一旦开启不可回退，因此同步基准在前、异步基准最后跑。
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "logger/logger.h"
#include "logger/sink/file_sink.h"

namespace {

using Clock = std::chrono::steady_clock;

// 丢弃所有输出的 Sink，用于隔离纯日志路径开销
class NullSink : public LogSink {
 public:
  bool log(const FormatResult&) override {
    return true;
  }
  void flush() override {}
};

struct Result {
  const char* name;
  double ns_per_op;
  double ops_per_sec;
  double p50;
  double p95;
  double p99;
};

double now_ns(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double, std::nano>(b - a).count();
}

void percentiles(std::vector<double>& lat, double& p50, double& p95, double& p99) {
  std::sort(lat.begin(), lat.end());
  auto at = [&](double p) { return lat[static_cast<size_t>(p * (lat.size() - 1))]; };
  p50 = at(0.50);
  p95 = at(0.95);
  p99 = at(0.99);
}

// 先 warmup，再测吞吐，最后单独采样延迟（避免逐次计时污染吞吐）
template <typename Fn>
Result run(const char* name, int iterations, int latency_samples, Fn&& fn) {
  for (int i = 0; i < iterations / 10; ++i)
    fn();

  auto t0 = Clock::now();
  for (int i = 0; i < iterations; ++i)
    fn();
  auto t1 = Clock::now();
  double ns_per_op = now_ns(t0, t1) / iterations;
  double ops_per_sec = 1e9 / ns_per_op;

  std::vector<double> lat;
  lat.reserve(latency_samples);
  for (int i = 0; i < latency_samples; ++i) {
    auto s = Clock::now();
    fn();
    auto e = Clock::now();
    lat.push_back(now_ns(s, e));
  }
  double p50, p95, p99;
  percentiles(lat, p50, p95, p99);
  return Result{name, ns_per_op, ops_per_sec, p50, p95, p99};
}

void print_row(const Result& r) {
  std::printf("%-22s %12.0f ns/op %14.0f ops/s   P50 %8.0f ns  P95 %8.0f ns  P99 %8.0f ns\n",
              r.name, r.ns_per_op, r.ops_per_sec, r.p50, r.p95, r.p99);
}

}  // namespace

int main() {
  std::printf("%-22s %12s %17s   %s\n", "benchmark", "ns/op", "ops/sec", "latency P50/P95/P99");
  std::printf(
      "---------------------------------------------"
      "---------------------------------------------\n");

  // 一个 NullSink 供所有同步/异步基准共享
  Logger::get_instance().add_sink(std::make_shared<NullSink>());

  // 1) 关闭级别：log_level = OFF，日志应在入口处尽早过滤
  {
    LogConfig cfg;
    cfg.log_level = LogLevel::OFF;
    Logger::get_instance().set_config(cfg);
    int x = 0;
    auto r = run("disabled_level", 5'000'000, 200'000, [&] { LOG_INFO("disabled {}", ++x); });
    print_row(r);
  }

  // 2) 同步文本
  {
    LogConfig cfg;
    cfg.log_level = LogLevel::TRACE;
    cfg.format = LogFormat::TEXT;
    Logger::get_instance().set_config(cfg);
    int x = 0;
    auto r = run("sync_text", 1'000'000, 200'000, [&] { LOG_INFO("bench message {}", ++x); });
    print_row(r);
  }

  // 3) 同步 JSON
  {
    LogConfig cfg;
    cfg.format = LogFormat::JSON;
    Logger::get_instance().set_config(cfg);
    int x = 0;
    auto r = run("sync_json", 500'000, 100'000,
                 [&] { Logger::get_instance().info("bench", KV("id", ++x), KV("name", "alice")); });
    print_row(r);
  }

  // 4) 带结构化字段的文本
  {
    LogConfig cfg;
    cfg.format = LogFormat::TEXT;
    Logger::get_instance().set_config(cfg);
    int x = 0;
    auto r = run("sync_with_fields", 1'000'000, 200'000,
                 [&] { Logger::get_instance().info("bench", KV("id", ++x), KV("name", "alice")); });
    print_row(r);
  }

  // 5) 并发：4 线程同时同步写，聚合吞吐
  {
    LogConfig cfg;
    cfg.format = LogFormat::TEXT;
    Logger::get_instance().set_config(cfg);
    const int kThreads = 4;
    const int kPerThread = 250'000;
    const int kTotal = kThreads * kPerThread;

    auto launch = [&](int n, int) {
      std::atomic<bool> go{false};
      std::vector<std::thread> ths;
      ths.reserve(kThreads);
      for (int t = 0; t < kThreads; ++t)
        ths.emplace_back([&, t] {
          while (!go.load(std::memory_order_acquire))
            std::this_thread::yield();
          for (int i = 0; i < n; ++i)
            LOG_INFO("concurrent {} {}", t, i);
        });
      go.store(true, std::memory_order_release);
      auto t0 = Clock::now();
      for (auto& th : ths)
        th.join();
      auto t1 = Clock::now();
      return now_ns(t0, t1);
    };

    launch(kPerThread / 10, 0);  // warmup
    double total_ns = launch(kPerThread, 0);
    double ns_per_op = total_ns / kTotal;
    double ops_per_sec = 1e9 / ns_per_op;
    std::printf("%-22s %12.0f ns/op %14.0f ops/s   (4 threads aggregate)\n", "concurrent_text",
                ns_per_op, ops_per_sec);
  }

  // 6) 异步文本（最后跑：异步一旦开启不可回退）。测业务线程入队开销
  {
    LogConfig cfg;
    cfg.async = true;
    cfg.buffer_size = 1u << 20;  // 大缓冲，避免生产者阻塞
    cfg.asy_que_ful_strategy = AsyQueFulStrategy::Block;
    cfg.format = LogFormat::TEXT;
    Logger::get_instance().set_config(cfg);
    int x = 0;
    auto r = run("async_text", 1'000'000, 200'000, [&] { LOG_INFO("bench message {}", ++x); });
    Logger::get_instance().flush_all();
    print_row(r);
  }

  // 7) 文件 Sink：直接测写盘开销（避免污染单例 sink）
  {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() /
                   ("logger_bench_" + std::to_string(Clock::now().time_since_epoch().count()));
    fs::create_directories(dir);

    FileSinkConfig fc;
    fc.dir = dir;
    fc.date_interval_h = 24;                   // 按天切分（基准内不会触发）
    fc.max_file_size = 1024ull * 1024 * 1024;  // 1GB，避免基准内触发大小轮转
    fc.max_backups = 0;
    FileSink sink(fc);

    FormatResult r;
    r.formatted_msg = std::string(80, 'x') + "\n";
    r.level = LogLevel::INFO;
    auto res = run("file_sink", 200'000, 20'000, [&] { sink.log(r); });
    sink.flush();
    print_row(res);

    std::error_code ec;
    fs::remove_all(dir, ec);
  }

  std::printf("\n注：并发为聚合吞吐；async_text 测业务线程入队开销（后台线程并发写 NullSink）。\n");
  return 0;
}
