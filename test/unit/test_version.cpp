#include <gtest/gtest.h>
#include <string>

#include "logger/version.h"

// 版本号：字符串与分量必须一致，且与 CMake 的 project(VERSION) 不漂移

// 宏不受命名空间约束，直接放文件作用域；用完 #undef，不污染其它用例
#define LOGGER_STRINGIFY_IMPL(x) #x
#define LOGGER_STRINGIFY(x) LOGGER_STRINGIFY_IMPL(x)

TEST(VersionTest, StringMatchesComponents) {
  const std::string from_parts = LOGGER_STRINGIFY(LOG_VERSION_MAJOR) "." LOGGER_STRINGIFY(
      LOG_VERSION_MINOR) "." LOGGER_STRINGIFY(LOG_VERSION_PATCH);
  EXPECT_EQ(std::string(LOG_VERSION), from_parts);
}

TEST(VersionTest, NumericFormMatchesComponents) {
  EXPECT_EQ(LOG_VERSION_CODE,
            LOG_VERSION_MAJOR * 10000 + LOG_VERSION_MINOR * 100 + LOG_VERSION_PATCH);
}

// CMake 把 project(VERSION) 透传进来。两个地方各写一份版本号，迟早会漂 —— 这条用例就是防它
TEST(VersionTest, MatchesCMakeProjectVersion) {
#ifdef LOGGER_CMAKE_VERSION
  EXPECT_EQ(std::string(LOG_VERSION), std::string(LOGGER_CMAKE_VERSION));
#else
  GTEST_SKIP() << "未定义 LOGGER_CMAKE_VERSION（非 CMake 构建）";
#endif
}

#undef LOGGER_STRINGIFY
#undef LOGGER_STRINGIFY_IMPL
