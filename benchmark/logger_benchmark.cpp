// 性能基准（Google Benchmark）。
//
// 用法：
//   cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
//   cmake --build build-release --target logger_benchmark
//   ./build-release/logger_benchmark                          # 全部
//   ./build-release/logger_benchmark --benchmark_filter=Sync  # 只跑一组
//   ./build-release/logger_benchmark --benchmark_repetitions=5 --benchmark_report_aggregates_only
//
// 每个场景输出两块数据：
//   1. 吞吐（ns/op、items/s）—— 由框架自适应迭代次数，多轮重复可看 CV
//   2. 延迟分位（P50_ns / P95_ns / P99_ns）—— 在计时循环**之后**单独采样，
//      框架本身只报均值与中位数，而 M4 的验收标准是 P99，必须自己测
//
// 全部基准都带 UseRealTime：否则 gbench 的 items_per_second 按 **CPU 时间**算，
// 多线程下会给出误导性的数（4 线程实测 3.4M/s，同一份工作按墙钟算是 12.8M/s）。
// 带 UseRealTime 时 Iterations 是**所有线程的总和**，Time 是聚合墙钟 ns/op。
//
// 两个已知的口径限制（见 docs/performance.md）：
//   - 系统包 libbenchmark 是按 DEBUG 编的（Ubuntu 打包如此），会打一行警告，
//     框架自身开销略高于 Release 版 gbench
//   - 延迟分位含两次 steady_clock::now()（约 20ns），对 ns 级场景没有分辨力，
//     只对 µs 级以上的场景有意义
//
// 对照 docs/milestone.md 的 M4 目标：关闭级别开销尽量低、同步文本 ≥ 100K 条/秒、
// 异步文本 ≥ 300K 条/秒、关键路径 P99 < 1ms。
//
// 注意：Logger 是单例，异步一旦开启不可回退。因此异步基准写在文件最后，
// 保证它运行时其它基准已经跑完；用 --benchmark_filter 只跑子集也不会互相污染。
#include <algorithm>
#include <atomic>
#include <benchmark/benchmark.h>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "logger/logger.h"
#include "logger/sink/file_sink.h"

using namespace logger;  // 库的公共符号都在 logger:: 下

namespace {

using Clock = std::chrono::steady_clock;

/// 丢弃所有输出的 Sink：隔离日志路径本身的开销，不含真实 I/O
class NullSink : public LogSink {
 public:
  bool log(const SinkInput&) override {
    return true;
  }
  void flush() override {}
};

double elapsed_ns(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double, std::nano>(b - a).count();
}

/// 采样单次调用延迟并写入 P50/P95/P99 计数器。
///
/// 放在计时循环之后：框架的计时区间是那个 for 循环，循环外的工作不计入吞吐，
/// 因此逐次计时不会污染吞吐数字（这正是旧版把两者分开的原因）。
template <typename Fn>
void report_latency(benchmark::State& state, Fn&& fn, int samples = 20000) {
  if (state.threads() > 1 || state.iterations() == 0)
    return;  // 多线程下逐次计时会互相干扰，不报分位

  std::vector<double> lat;
  lat.reserve(static_cast<std::size_t>(samples));
  for (int i = 0; i < samples; ++i) {
    const auto t0 = Clock::now();
    fn();
    const auto t1 = Clock::now();
    lat.push_back(elapsed_ns(t0, t1));
  }
  std::sort(lat.begin(), lat.end());
  const auto at = [&](double p) { return lat[static_cast<std::size_t>(p * (lat.size() - 1))]; };
  state.counters["P50_ns"] = at(0.50);
  state.counters["P95_ns"] = at(0.95);
  state.counters["P99_ns"] = at(0.99);
}

/// 装配一份配置并生效。在计时循环之前调用（框架从循环开始计时）。
void use_config(const LogConfig& cfg) {
  Logger::get_instance().set_config(cfg);
}

LogConfig text_config() {
  LogConfig cfg;
  cfg.log_level = LogLevel::TRACE;
  cfg.format = LogFormat::TEXT;
  return cfg;
}

// ---------------------------------------------------------------------------
// 基础路径
// ---------------------------------------------------------------------------

/// 级别关闭：应在入口处无锁早退，开销接近一次函数调用。
static void BM_DisabledLevel(benchmark::State& state) {
  LogConfig cfg = text_config();
  cfg.log_level = LogLevel::OFF;
  use_config(cfg);

  int x = 0;
  for (auto _ : state) {
    LOG_INFO("disabled {}", ++x);
  }
  state.SetItemsProcessed(state.iterations());
  benchmark::DoNotOptimize(x);
  report_latency(state, [&] { LOG_INFO("disabled {}", ++x); });
}
BENCHMARK(BM_DisabledLevel)->Unit(benchmark::kNanosecond)->UseRealTime();

/// 同步文本：完整链路（格式化 + 写 Sink），Sink 为 NullSink。
static void BM_SyncText(benchmark::State& state) {
  use_config(text_config());

  int x = 0;
  for (auto _ : state) {
    LOG_INFO("bench message {}", ++x);
  }
  state.SetItemsProcessed(state.iterations());
  report_latency(state, [&] { LOG_INFO("bench message {}", ++x); });
}
BENCHMARK(BM_SyncText)->Unit(benchmark::kNanosecond)->UseRealTime();

/// 同步 JSON：与文本同一路径，只有字符串分支不同。
static void BM_SyncJson(benchmark::State& state) {
  LogConfig cfg = text_config();
  cfg.format = LogFormat::JSON;
  use_config(cfg);

  int x = 0;
  for (auto _ : state) {
    Logger::get_instance().info("bench", KV("id", ++x), KV("name", "alice"));
  }
  state.SetItemsProcessed(state.iterations());
  report_latency(state,
                 [&] { Logger::get_instance().info("bench", KV("id", ++x), KV("name", "alice")); });
}
BENCHMARK(BM_SyncJson)->Unit(benchmark::kNanosecond)->UseRealTime();

/// 带两个结构化字段的文本。
static void BM_SyncWithFields(benchmark::State& state) {
  use_config(text_config());

  int x = 0;
  for (auto _ : state) {
    Logger::get_instance().info("bench", KV("id", ++x), KV("name", "alice"));
  }
  state.SetItemsProcessed(state.iterations());
  report_latency(state,
                 [&] { Logger::get_instance().info("bench", KV("id", ++x), KV("name", "alice")); });
}
BENCHMARK(BM_SyncWithFields)->Unit(benchmark::kNanosecond)->UseRealTime();

// ---------------------------------------------------------------------------
// M5 / M6 新增能力的开销（在此之前从未测过）
// ---------------------------------------------------------------------------

/// 作用域上下文：每条的字段合并开销（2 个上下文字段 + 1 个调用点字段）。
static void BM_SyncWithContext(benchmark::State& state) {
  use_config(text_config());

  int x = 0;
  for (auto _ : state) {
    ContextScope ctx{KV("request_id", x), KV("user_id", "u-1")};
    LOG_INFO("with context {}", ++x);
  }
  state.SetItemsProcessed(state.iterations());
  report_latency(state, [&] {
    ContextScope ctx{KV("request_id", x), KV("user_id", "u-1")};
    LOG_INFO("with context {}", ++x);
  });
}
BENCHMARK(BM_SyncWithContext)->Unit(benchmark::kNanosecond)->UseRealTime();

/// 脱敏开启：3 个字段里 1 个命中关键词表，对比 BM_SyncWithFields 看净开销。
static void BM_SyncWithMasking(benchmark::State& state) {
  LogConfig cfg = text_config();
  cfg.enable_sensitive_field_mask = true;
  use_config(cfg);

  int x = 0;
  for (auto _ : state) {
    Logger::get_instance().info("login", KV("user", "alice"), KV("password", "s3cret"),
                                KV("id", ++x));
  }
  state.SetItemsProcessed(state.iterations());
  report_latency(state, [&] {
    Logger::get_instance().info("login", KV("user", "alice"), KV("password", "s3cret"),
                                KV("id", ++x));
  });
}
BENCHMARK(BM_SyncWithMasking)->Unit(benchmark::kNanosecond)->UseRealTime();

/// 聚合去重：同一条日志反复打，命中抑制路径（判定在字段合并之前，开销应远低于正常路径）。
static void BM_SyncWithDedup(benchmark::State& state) {
  LogConfig cfg = text_config();
  cfg.dedup_window_ms = 1000;
  use_config(cfg);

  int x = 0;
  for (auto _ : state) {
    LOG_INFO("repeated message {}", ++x);  // 注意：x 变了仍算同一条（键是 file:line）
  }
  state.SetItemsProcessed(state.iterations());
  report_latency(state, [&] { LOG_INFO("repeated message {}", ++x); });
  Logger::get_instance().flush_all();  // 补发摘要，避免把待发状态留给下一个基准
}
BENCHMARK(BM_SyncWithDedup)->Unit(benchmark::kNanosecond)->UseRealTime();

/// 长度预算触发裁剪：字段被丢掉、消息被截断时的降级路径。
static void BM_SyncWithLimits(benchmark::State& state) {
  LogConfig cfg = text_config();
  cfg.max_record_size = 256;
  use_config(cfg);

  const std::string long_text(512, 'x');
  int x = 0;
  for (auto _ : state) {
    Logger::get_instance().info(long_text.c_str(), KV("a", ++x), KV("b", long_text),
                                KV("c", long_text));
  }
  state.SetItemsProcessed(state.iterations());
  report_latency(state, [&] {
    Logger::get_instance().info(long_text.c_str(), KV("a", ++x), KV("b", long_text),
                                KV("c", long_text));
  });
}
BENCHMARK(BM_SyncWithLimits)->Unit(benchmark::kNanosecond)->UseRealTime();

/// 内容需要转义：走文本转义的慢路径，对比 BM_SyncText 的快路径。
static void BM_SyncWithEscape(benchmark::State& state) {
  use_config(text_config());

  int x = 0;
  for (auto _ : state) {
    LOG_INFO("line1\nline2\ttab \"quoted\" path\\to\\file {}", ++x);
  }
  state.SetItemsProcessed(state.iterations());
  report_latency(state, [&] { LOG_INFO("line1\nline2\ttab \"quoted\" path\\to\\file {}", ++x); });
}
BENCHMARK(BM_SyncWithEscape)->Unit(benchmark::kNanosecond)->UseRealTime();

/// 异常记录：含嵌套链，测的是 error / error_type / error_chain 的提取开销
/// （demangle + rethrow_if_nested 走链）。异常对象在循环外构造好，只测提取。
static void BM_SyncException(benchmark::State& state) {
  use_config(text_config());

  std::exception_ptr ep;
  try {
    try {
      throw std::runtime_error("inner: connection refused");
    } catch (...) {
      std::throw_with_nested(std::logic_error("outer: query failed"));
    }
  } catch (...) {
    ep = std::current_exception();
  }

  for (auto _ : state) {
    Logger::get_instance().log_exception(LogLevel::ERROR, __FILE__, __LINE__, __func__, "bench",
                                         ep);
  }
  state.SetItemsProcessed(state.iterations());
  report_latency(state, [&] {
    Logger::get_instance().log_exception(LogLevel::ERROR, __FILE__, __LINE__, __func__, "bench",
                                         ep);
  });
}
BENCHMARK(BM_SyncException)->Unit(benchmark::kNanosecond)->UseRealTime();

/// 只采集不符号化：capture() 只调 backtrace()，应当很便宜。
static void BM_StacktraceCapture(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(StackTrace::capture());
  }
  state.SetItemsProcessed(state.iterations());
  report_latency(state, [] { benchmark::DoNotOptimize(StackTrace::capture()); });
}
BENCHMARK(BM_StacktraceCapture)->Unit(benchmark::kNanosecond)->UseRealTime();

/// 完整堆栈路径：采集 + 符号化（dladdr + demangle）+ 格式化。
static void BM_StacktraceFullPath(benchmark::State& state) {
  use_config(text_config());

  for (auto _ : state) {
    LOG_ERROR("bench", KV("stacktrace", StackTrace::capture()));
  }
  state.SetItemsProcessed(state.iterations());
  report_latency(state, [] { LOG_ERROR("bench", KV("stacktrace", StackTrace::capture())); });
}
BENCHMARK(BM_StacktraceFullPath)->Unit(benchmark::kNanosecond)->UseRealTime();

/// 并发同步写：4 线程，框架报的是聚合吞吐。
static void BM_ConcurrentText(benchmark::State& state) {
  use_config(text_config());

  int x = 0;
  for (auto _ : state) {
    LOG_INFO("concurrent {} {}", static_cast<int>(state.thread_index()), ++x);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ConcurrentText)->Unit(benchmark::kNanosecond)->UseRealTime()->Threads(4);

// ---------------------------------------------------------------------------
// 文件 Sink
// ---------------------------------------------------------------------------

std::filesystem::path g_bench_dir;
std::unique_ptr<FileSink> g_file_sink;

void file_sink_setup(const benchmark::State&) {
  namespace fs = std::filesystem;
  g_bench_dir = fs::temp_directory_path() /
                ("logger_bench_" + std::to_string(Clock::now().time_since_epoch().count()));
  fs::create_directories(g_bench_dir);

  FileSinkConfig fc;
  fc.dir = g_bench_dir;
  fc.date_interval_h = 24;                   // 基准内不会触发时间切分
  fc.max_file_size = 1024ull * 1024 * 1024;  // 1GB，避免基准内触发大小轮转
  fc.max_backups = 0;
  g_file_sink = std::make_unique<FileSink>(fc);
}

void file_sink_teardown(const benchmark::State&) {
  g_file_sink.reset();
  std::error_code ec;
  std::filesystem::remove_all(g_bench_dir, ec);
}

/// 文件写入本身：格式化好的成品交给 ofstream，测的是 Sink 与文件系统。
static void BM_FileSinkWrite(benchmark::State& state) {
  SinkInput input;
  input.formatted_msg = std::string(80, 'x') + "\n";
  input.level = LogLevel::INFO;

  for (auto _ : state) {
    benchmark::DoNotOptimize(g_file_sink->log(input));
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FileSinkWrite)
    ->Unit(benchmark::kNanosecond)
    ->UseRealTime()
    ->Setup(file_sink_setup)
    ->Teardown(file_sink_teardown);

// ---------------------------------------------------------------------------
// 异步（必须最后：异步一旦开启不可回退）
// ---------------------------------------------------------------------------

/// 业务线程入队开销（后台线程并发写 NullSink）。
///
/// 用固定迭代次数而不是自适应：队列在基准期间持续增长，跑到 min_time 会堆出
/// 几百 MB 记录。固定 20 万条把内存压在几十 MB 量级。
static void BM_AsyncEnqueue(benchmark::State& state) {
  LogConfig cfg = text_config();
  cfg.async = true;
  cfg.buffer_size = 1u << 20;  // 大缓冲，避免生产者被 Block 策略挡住
  cfg.asy_que_ful_strategy = AsyQueFulStrategy::Block;
  use_config(cfg);

  int x = 0;
  for (auto _ : state) {
    LOG_INFO("bench message {}", ++x);
  }
  state.SetItemsProcessed(state.iterations());
  report_latency(state, [&] { LOG_INFO("bench message {}", ++x); });
  Logger::get_instance().flush_all();
}
BENCHMARK(BM_AsyncEnqueue)->Unit(benchmark::kNanosecond)->UseRealTime()->Iterations(200000);

/// 队列满时的丢弃路径：小队列 + DropNewest，队列恒定满，测的是判定与计数开销。
static void BM_AsyncQueueFullDrop(benchmark::State& state) {
  LogConfig cfg = text_config();
  cfg.async = true;
  cfg.buffer_size = 64;
  cfg.asy_que_ful_strategy = AsyQueFulStrategy::DropNewest;
  use_config(cfg);

  int x = 0;
  for (auto _ : state) {
    LOG_INFO("bench message {}", ++x);
  }
  state.SetItemsProcessed(state.iterations());
  Logger::get_instance().flush_all();
}
BENCHMARK(BM_AsyncQueueFullDrop)->Unit(benchmark::kNanosecond)->UseRealTime()->Iterations(200000);

}  // namespace
