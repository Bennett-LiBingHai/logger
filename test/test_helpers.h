#pragma once
#include <condition_variable>
#include <mutex>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>

#include "logger/formatter.h"
#include "logger/sink.h"

// 捕获日志输出的测试用 Sink（线程安全），用于断言 Logger 实际写出的内容。
class CapturingSink : public LogSink {
 public:
  bool log(const FormatResult& result) override {
    std::lock_guard<std::mutex> lock(mtx_);
    messages_.push_back(result.formatted_msg);
    return true;
  }

  void flush() override {
    std::lock_guard<std::mutex> lock(mtx_);
    ++flush_count_;
  }

  std::vector<std::string> messages() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return messages_;
  }

  size_t size() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return messages_.size();
  }

  int flush_count() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return flush_count_;
  }

 private:
  mutable std::mutex mtx_;
  std::vector<std::string> messages_;
  int flush_count_ = 0;
};

// 只失败一次的 Sink，用于测试失败策略（避免污染单例后续用例）
class FailOnceSink : public LogSink {
 public:
  bool log(const FormatResult& /*result*/) override {
    if (failed_)
      return true;
    failed_ = true;
    return false;
  }
  void flush() override {}

 private:
  bool failed_ = false;
};

// 门控 Sink：log() 先记录消息，再阻塞直到 open()。
// block_first 指定前 N 次 log() 调用阻塞（默认全部阻塞），用于队列满测试中卡住后台线程。
class GateSink : public LogSink {
 public:
  explicit GateSink(size_t block_first = ~size_t{0}) : block_first_(block_first) {}

  bool log(const FormatResult& result) override {
    {
      std::lock_guard<std::mutex> lock(mtx_);
      messages_.push_back(result.formatted_msg);
      ++received_;
      received_cv_.notify_all();
    }
    bool should_block = false;
    {
      std::lock_guard<std::mutex> lock(call_mtx_);
      should_block = (call_++ < block_first_);
    }
    if (should_block) {
      std::unique_lock<std::mutex> gate_lock(gate_mtx_);
      gate_cv_.wait(gate_lock, [this] { return open_; });
    }
    return true;
  }

  void flush() override {}

  // 放行所有被阻塞的 log() 调用
  void open() {
    {
      std::lock_guard<std::mutex> lock(gate_mtx_);
      open_ = true;
    }
    gate_cv_.notify_all();
  }

  // 等待已记录（含被阻塞的）消息数达到 n
  void wait_received(size_t n) {
    std::unique_lock<std::mutex> lock(mtx_);
    received_cv_.wait(lock, [this, n] { return received_ >= n; });
  }

  size_t size() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return messages_.size();
  }

  std::vector<std::string> messages() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return messages_;
  }

  bool contains(const std::string& needle) const {
    std::lock_guard<std::mutex> lock(mtx_);
    for (const auto& m : messages_)
      if (m.find(needle) != std::string::npos)
        return true;
    return false;
  }

 private:
  mutable std::mutex mtx_;
  std::vector<std::string> messages_;
  size_t received_ = 0;
  std::condition_variable received_cv_;

  std::mutex call_mtx_;
  size_t call_ = 0;
  size_t block_first_;

  std::mutex gate_mtx_;
  std::condition_variable gate_cv_;
  bool open_ = false;
};

// 只抛异常一次的 Sink，用于测试日志不因 sink 异常而崩溃
class ThrowOnceSink : public LogSink {
 public:
  bool log(const FormatResult& /*result*/) override {
    if (thrown_)
      return true;
    thrown_ = true;
    throw std::runtime_error("boom");
  }
  void flush() override {}

 private:
  bool thrown_ = false;
};

// M1 文本输出格式（与 TextFormatter 一致）：
//   YYYY-MM-DDTHH:MM:SS.mmm[LEVEL][thread_id][file:line]content\n
inline bool matches_log_line(const std::string& line) {
  static const std::regex re(
      R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}\[[A-Z]+\]\[[^\]]*\]\[[^\]]*:\d+\].*\n$)");
  return std::regex_match(line, re);
}
