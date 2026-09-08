#pragma once
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "logger/sink.h"

// 文件日志配置
struct FileSinkConfig {
  std::filesystem::path dir;          // 日志目录
  unsigned int date_interval_h = 24;  // 按时间切分间隔（0 = 只按大小）
  unsigned long long max_file_size = 10 * 1024 * 1024;  // 单文件最大字节（0 = 只按时间）
  unsigned int max_backups = 100;                       // 最多保留文件数（0 = 不删除）
};

// 文件日志输出槽（指定目录，自动按日期/大小轮转，限制文件数）
// date_interval_h 为 0 表示只按大小切分；max_file_size 为 0 表示只按时间切分
class FileSink : public LogSink {
 public:
  explicit FileSink(const FileSinkConfig& config);

  // 打印日志，返回是否写入成功（失败由 Logger 按策略兜底）
  bool log(const FormatResult& result) override;

  // 刷新日志缓冲区
  void flush() override;

 private:
  // 检查是否需要轮转/重开文件
  void check() noexcept;
  // 轮转：change_date=true 按日期切分，false 按大小切分
  void rotate(bool change_date) noexcept;
  // 打开当前 file_，返回是否成功
  bool open_file() noexcept;
  // 限制文件数，超出删除最旧的
  void limit_log_files() noexcept;

  FileSinkConfig config_;                           // 配置
  std::filesystem::path file_;                      // 当前日志文件路径（含 .N 后缀）
  std::filesystem::path base_;                      // 日期基准路径（不含 .N）
  std::ofstream ofs_;                               // 日志文件流
  int n_ = 0;                                       // 同日超尺寸计数
  unsigned long long written_ = 0;                  // 当前文件已写字节数
  std::chrono::system_clock::time_point pre_time_;  // 上次创建文件的时间
};
