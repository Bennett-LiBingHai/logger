#include <chrono>
#include <filesystem>
#include <gtest/gtest.h>
#include <iterator>
#include <memory>
#include <string>
#include <unistd.h>

#include "logger/logger.h"
#include "logger/sink/file_sink.h"

#include "test_helpers.h"

using namespace logger;          // 库的公共符号
using namespace logger::detail;  // 白盒用例要直接构造 Record / Formatter 等内部类型

// M7：文件系统异常 —— 权限、目录消失、写失败降级、fd 泄漏
//
// 未覆盖：ENOSPC（磁盘满）。要真实触发需要 root 挂一个很小的 tmpfs；
// 写失败路径本身由「只读目录」与 test_helpers 里的 FailOnceSink 覆盖。

namespace fs = std::filesystem;

namespace {

SinkInput make_result(const std::string& msg) {
  SinkInput r;
  r.formatted_msg = msg + "\n";
  r.level = LogLevel::INFO;
  return r;
}

struct TempDir {
  fs::path path;
  TempDir() {
    path = fs::temp_directory_path() /
           ("logger_io_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::permissions(path, fs::perms::owner_all, ec);  // 还原权限，否则删不掉
    fs::remove_all(path, ec);
  }
};

// 目录只读（去掉写权限）
void make_read_only(const fs::path& p) {
  std::error_code ec;
  fs::permissions(p, fs::perms::owner_read | fs::perms::owner_exec, ec);
}

// 当前进程打开的 fd 数（读 /proc/self/fd 的条目数）
size_t open_fd_count() {
  std::error_code ec;
  size_t n = 0;
  for (auto it = fs::directory_iterator("/proc/self/fd", ec); it != fs::directory_iterator();
       it.increment(ec)) {
    if (ec)
      break;
    ++n;
  }
  return n;
}

}  // namespace

// root 会绕过权限检查，跳过相关用例
#define SKIP_IF_ROOT()                            \
  if (::geteuid() == 0) {                         \
    GTEST_SKIP() << "以 root 运行，权限位不生效"; \
  }

TEST(IoFailureTest, ReadOnlyDirMakesFileSinkReportFailure) {
  SKIP_IF_ROOT();
  TempDir tmp;
  make_read_only(tmp.path);

  FileSinkConfig cfg;
  cfg.dir = tmp.path;
  FileSink sink(cfg);
  EXPECT_FALSE(sink.log(make_result("x")));  // 打不开文件，如实返回失败
}

TEST(IoFailureTest, LoggerDegradesWhenFileSinkFails) {
  SKIP_IF_ROOT();
  TempDir tmp;
  make_read_only(tmp.path);

  auto capture = std::make_shared<CapturingSink>();
  Logger::get_instance().add_sink(capture);
  Logger::get_instance().add_sink(std::make_shared<FileSink>(FileSinkConfig{tmp.path}));

  LogConfig cfg;
  cfg.log_fail_strategy = LogFailStrategy::Drop;  // 写失败即丢弃
  Logger::get_instance().set_config(cfg);

  const auto before = Logger::get_instance().stats();
  Logger::get_instance().info("hello");
  const auto after = Logger::get_instance().stats();

  EXPECT_EQ(after.failed_writes - before.failed_writes, 1u);  // FileSink 失败一次
  EXPECT_EQ(after.dropped - before.dropped, 1u);
  EXPECT_GE(after.written - before.written, 1u);  // 另一个 sink 成功了
  EXPECT_EQ(capture->size(), 1u);                 // 日志没有因为坏 sink 而丢失
}

TEST(IoFailureTest, FailsWhenDirRemovedAtRuntime) {
  TempDir tmp;
  FileSinkConfig cfg;
  cfg.dir = tmp.path;
  FileSink sink(cfg);
  ASSERT_TRUE(sink.log(make_result("first")));

  std::error_code ec;
  fs::remove_all(tmp.path, ec);  // 目录整个被删（比"文件被删"更狠）

  EXPECT_FALSE(sink.log(make_result("second")));  // 无法重开
}

TEST(IoFailureTest, FileRotationDoesNotLeakFileDescriptors) {
  const size_t before = open_fd_count();

  for (int round = 0; round < 20; ++round) {
    TempDir tmp;
    FileSinkConfig cfg;
    cfg.dir = tmp.path;
    cfg.date_interval_h = 0;  // 只按大小切，制造大量轮转
    cfg.max_file_size = 64;
    cfg.max_backups = 2;
    FileSink sink(cfg);
    for (int i = 0; i < 50; ++i)
      sink.log(make_result(std::string(40, 'x')));
    sink.flush();
  }

  EXPECT_LE(open_fd_count(), before + 2) << "反复轮转后 fd 数量不应增长";
}
