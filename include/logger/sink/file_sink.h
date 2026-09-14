#pragma once
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "logger/sink.h"

/// @brief 文件输出槽的配置。
///
/// 轮转按「日期」与「大小」两条轴各自独立触发，可只开一条：
/// date_interval_h 为 0 表示只按大小切分，max_file_size 为 0 表示只按时间切分。
struct FileSinkConfig {
  std::filesystem::path dir;  ///< 日志目录；不存在会自动创建，创建失败则写入失败
  unsigned int date_interval_h = 24;  ///< 按时间切分的间隔（小时），0 = 只按大小
  unsigned long long max_file_size = 10ull * 1024 * 1024;  ///< 单文件最大字节，0 = 只按时间
  unsigned int max_backups = 100;  ///< 最多保留的文件数，0 = 不删除
};

/// @brief 文件输出槽：按日期 / 大小轮转，并限制保留的文件数。
///
/// 轮转后的文件名形如 `app.log.1`、`app.log.2`；按日期切分时基准名带日期，
/// 例如 `2026_09_13_14.log`。超出 max_backups 时删除最旧的。
///
/// @par 多进程
/// 不保证 fork 安全，也不做跨进程的轮转协调——约定单进程单文件。多进程部署时
/// 用 pid / 主机名区分文件路径。
///
/// @note 本类不是线程安全的；Logger 在写 Sink 时会加锁，因此无需自行同步。
class FileSink : public LogSink {
 public:
  /// @brief 构造并打开日志文件。
  /// @param config 文件与轮转配置。
  explicit FileSink(const FileSinkConfig& config);

  /// @brief 写入一条日志；必要时先检查轮转。
  /// @param result 格式化成品。
  /// @return 是否写入成功；失败时由 Logger 按 LogFailStrategy 兜底。
  bool log(const SinkInput& result) override;

  /// @brief 刷新文件流缓冲。
  void flush() override;

 private:
  /// @brief 检查是否需要轮转或重新打开文件（文件被外部删除 / 重命名时）。
  void check() noexcept;

  /// @brief 执行轮转并打开新文件。
  /// @param change_date true 按日期切分，false 按大小切分。
  void rotate(bool change_date) noexcept;

  /// @brief 打开当前 file_。
  /// @return 是否成功。
  bool open_file() noexcept;

  /// @brief 按 max_backups 清理最旧的文件。
  void limit_log_files() noexcept;

  FileSinkConfig config_;                           ///< 配置
  std::filesystem::path file_;                      ///< 当前日志文件路径（含 .N 后缀）
  std::filesystem::path base_;                      ///< 日期基准路径（不含 .N）
  std::ofstream ofs_;                               ///< 日志文件流
  int n_ = 0;                                       ///< 同一日期内的超尺寸序号
  unsigned long long written_ = 0;                  ///< 当前文件已写字节数
  std::chrono::system_clock::time_point pre_time_;  ///< 上次创建文件的时间
};
