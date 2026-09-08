#include "logger/sink/file_sink.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <system_error>
#include <vector>

#include "logger/utiils.h"

FileSink::FileSink(const FileSinkConfig& config) : config_(config) {
  if (config_.dir.empty())
    return;  // 无效目录：保持不可用，写失败由 Logger 按策略兜底

  // 目录不存在则尝试创建；失败也不崩溃
  std::error_code ec;
  if (!std::filesystem::exists(config_.dir, ec)) {
    std::filesystem::create_directories(config_.dir, ec);
  }
  rotate(true);
}

// 打印日志，返回是否写入成功
bool FileSink::log(const FormatResult& result) {
  check();
  if (!ofs_.is_open())
    return false;
  ofs_ << result;
  if (ofs_.fail()) {
    ofs_.clear();
    return false;
  }
  written_ += result.formatted_msg.size();
  return true;
}

// 刷新日志缓冲区
void FileSink::flush() {
  ofs_.flush();
}

// 打开当前 file_，返回是否成功
bool FileSink::open_file() noexcept {
  ofs_.close();
  ofs_.clear();
  ofs_.open(file_, std::ios::out | std::ios::app);
  if (ofs_.is_open()) {
    pre_time_ = std::chrono::system_clock::now();
    std::error_code ec;
    auto size = std::filesystem::file_size(file_, ec);
    written_ = ec ? 0 : static_cast<unsigned long long>(size);
    return true;
  }
  return false;
}

// 轮转：计算新文件名并打开
void FileSink::rotate(bool change_date) noexcept {
  if (change_date) {
    char buf[64];
    time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
    if (!localtime_safe(t, tm))
      return;
    if (std::strftime(buf, sizeof(buf), "%Y_%m_%d_%H.log", &tm) == 0)
      return;
    base_ = config_.dir / buf;
    n_ = 0;
    file_ = base_;
  } else {
    ++n_;
    file_ = base_;
    file_ += "." + std::to_string(n_);
  }
  open_file();
}

// 检查是否需要轮转/重开文件
void FileSink::check() noexcept {
  // 句柄失效或文件被外部删除 → 重开当前文件
  std::error_code ec;
  if (!ofs_.is_open() || !std::filesystem::exists(file_, ec)) {
    open_file();
    return;
  }
  // 按时间切分
  if (config_.date_interval_h != 0) {
    auto interval = std::chrono::duration_cast<std::chrono::hours>(
        std::chrono::system_clock::now() - pre_time_);
    if (interval.count() >= static_cast<long long>(config_.date_interval_h)) {
      rotate(true);
      return;
    }
  }
  // 按大小切分（用逻辑写入字节数，避免 ofstream 缓冲导致 file_size 滞后）
  if (config_.max_file_size != 0 && written_ >= config_.max_file_size) {
    rotate(false);
    return;
  }
  // 限制文件数
  if (config_.max_backups != 0) {
    limit_log_files();
  }
}

// 限制文件数，超出删除最旧的
void FileSink::limit_log_files() noexcept {
  std::error_code ec;
  if (!std::filesystem::exists(config_.dir, ec))
    return;

  std::vector<std::filesystem::path> files;
  for (auto it = std::filesystem::directory_iterator(config_.dir, ec);
       it != std::filesystem::directory_iterator(); it.increment(ec)) {
    if (ec)
      return;
    if (it->is_regular_file())
      files.push_back(it->path());
  }
  if (files.size() <= config_.max_backups)
    return;

  // 按最后写入时间降序（最新的在前），删除超出部分
  std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
    std::error_code ea;
    std::error_code eb;
    return std::filesystem::last_write_time(a, ea) > std::filesystem::last_write_time(b, eb);
  });
  for (size_t i = config_.max_backups; i < files.size(); ++i) {
    std::filesystem::remove(files[i], ec);
    ec.clear();
  }
}
