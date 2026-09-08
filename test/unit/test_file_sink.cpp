#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <string>

#include "logger/formatter.h"
#include "logger/level.h"
#include "logger/sink/file_sink.h"

namespace fs = std::filesystem;

namespace {
FormatResult make_result(const std::string& msg) {
  FormatResult r;
  r.formatted_msg = msg + "\n";
  r.level = LogLevel::INFO;
  return r;
}

// 临时目录，析构时清理
struct TempDir {
  fs::path path;
  TempDir() {
    path = fs::temp_directory_path() /
           ("logger_test_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

size_t count_files(const fs::path& dir) {
  size_t n = 0;
  std::error_code ec;
  for (auto it = fs::directory_iterator(dir, ec); it != fs::directory_iterator();
       it.increment(ec)) {
    if (ec)
      return n;
    if (it->is_regular_file())
      ++n;
  }
  return n;
}

fs::path first_file(const fs::path& dir) {
  std::error_code ec;
  for (auto& p : fs::directory_iterator(dir, ec)) {
    if (p.is_regular_file())
      return p.path();
  }
  return {};
}
}  // namespace

TEST(FileSinkTest, WritesToFile) {
  TempDir tmp;
  FileSinkConfig cfg;
  cfg.dir = tmp.path;
  FileSink sink(cfg);

  ASSERT_TRUE(sink.log(make_result("hello")));
  sink.flush();
  ASSERT_EQ(count_files(tmp.path), 1u);

  std::ifstream in(first_file(tmp.path));
  std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_NE(content.find("hello"), std::string::npos);
}

TEST(FileSinkTest, RotatesBySize) {
  TempDir tmp;
  FileSinkConfig cfg;
  cfg.dir = tmp.path;
  cfg.date_interval_h = 0;  // 只按大小
  cfg.max_file_size = 40;
  cfg.max_backups = 0;  // 不删除
  FileSink sink(cfg);

  std::string msg(30, 'x');
  for (int i = 0; i < 20; ++i)
    sink.log(make_result(msg));
  sink.flush();

  EXPECT_GE(count_files(tmp.path), 2u);
}

TEST(FileSinkTest, LimitsBackups) {
  TempDir tmp;
  FileSinkConfig cfg;
  cfg.dir = tmp.path;
  cfg.date_interval_h = 0;
  cfg.max_file_size = 40;
  cfg.max_backups = 2;
  FileSink sink(cfg);

  std::string msg(30, 'x');
  for (int i = 0; i < 30; ++i)
    sink.log(make_result(msg));
  sink.flush();

  EXPECT_LE(count_files(tmp.path), cfg.max_backups + 1);
}

TEST(FileSinkTest, ReopensAfterExternalDelete) {
  TempDir tmp;
  FileSinkConfig cfg;
  cfg.dir = tmp.path;
  FileSink sink(cfg);

  ASSERT_TRUE(sink.log(make_result("first")));
  sink.flush();
  ASSERT_EQ(count_files(tmp.path), 1u);

  // 外部删除当前文件
  fs::path f = first_file(tmp.path);
  std::error_code ec;
  fs::remove(f, ec);
  ASSERT_FALSE(fs::exists(f, ec));

  // 再次写入应重新打开
  EXPECT_TRUE(sink.log(make_result("second")));
  EXPECT_EQ(count_files(tmp.path), 1u);
}

TEST(FileSinkTest, ReturnsFalseWhenDirInvalid) {
  FileSinkConfig cfg;  // dir 为空 → 不可用
  FileSink sink(cfg);
  EXPECT_FALSE(sink.log(make_result("x")));
}
