#pragma once

/// @file literals.h
/// @brief 字节单位字面量后缀，用于可读地书写大小限制。

/// @brief 字节单位字面量。
///
/// @code
/// FileSinkConfig cfg;
/// cfg.max_file_size = 10_mb;
/// cfg.max_backups = 5;
/// @endcode
namespace logger::literals {

/// @brief 字节。
/// @param n 数值。
/// @return 等价字节数（原样返回）。
constexpr unsigned long long operator""_b(unsigned long long n) {
  return n;
}

/// @brief 千字节（KiB，即 1024 字节）。
/// @param n 数值。
/// @return 等价字节数。
constexpr unsigned long long operator""_kb(unsigned long long n) {
  return n * 1024ULL;
}

/// @brief 兆字节（MiB，即 1024 KiB）。
/// @param n 数值。
/// @return 等价字节数。
constexpr unsigned long long operator""_mb(unsigned long long n) {
  return n * 1024ULL * 1024ULL;
}

/// @brief 吉字节（GiB，即 1024 MiB）。
/// @param n 数值。
/// @return 等价字节数。
constexpr unsigned long long operator""_gb(unsigned long long n) {
  return n * 1024ULL * 1024ULL * 1024ULL;
}
}  // namespace logger::literals
