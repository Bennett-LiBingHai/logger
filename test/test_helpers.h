#pragma once
#include <mutex>
#include <regex>
#include <string>
#include <vector>

#include "logger/formatter.h"
#include "logger/sink.h"

// 捕获日志输出的测试用 Sink（线程安全），用于断言 Logger 实际写出的内容。
class CapturingSink : public LogSink {
 public:
  void log(const FormatResult& result) override {
    std::lock_guard<std::mutex> lock(mtx_);
    messages_.push_back(result.formatted_msg);
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

// M1 文本输出格式（与 TextFormatter 一致）：
//   YYYY-MM-DDTHH:MM:SS.mmm[LEVEL][thread_id][file:line]content\n
inline bool matches_log_line(const std::string& line) {
  static const std::regex re(
      R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}\[[A-Z]+\]\[[^\]]*\]\[[^\]]*:\d+\].*\n$)");
  return std::regex_match(line, re);
}
