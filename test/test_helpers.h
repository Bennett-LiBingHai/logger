#pragma once
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
