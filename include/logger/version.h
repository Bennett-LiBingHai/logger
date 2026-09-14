#pragma once

/// @file version.h
/// @brief 库的版本号。
///
/// 自 v1.0.0 起按[语义化版本](https://semver.org/lang/zh-CN/)承诺兼容性：
/// 破坏性变更只在主版本号递增时发生，且提供迁移方法（见 docs/compatibility.md）。
///
/// @par 典型用法
/// @code
/// LOG_INFO("logger {} starting", LOG_VERSION);          // 启动时留一行版本，便于排障
/// #if LOG_VERSION_CODE >= 10100                          // 源码级兼容分支
///   ...
/// #endif
/// @endcode

/// @defgroup version 版本号
/// 三个分量与字符串、数值形式；`LOG_VERSION_CODE` 用于 `#if` 比较。
/// @{

/// @brief 主版本号；有破坏性变更时递增。
#define LOG_VERSION_MAJOR 1

/// @brief 次版本号；有向后兼容的新增时递增。
#define LOG_VERSION_MINOR 0

/// @brief 修订号；只有向后兼容的修复时递增。
#define LOG_VERSION_PATCH 0

/// @brief 版本号字符串，形如 `"1.0.0"`。
#define LOG_VERSION "1.0.0"

/// @brief 版本号的数值形式，便于 `#if` 比较：`主 * 10000 + 次 * 100 + 修订`。
/// @code
/// #if LOG_VERSION_CODE >= 10100   // 需要 1.1.0 以上
/// #endif
/// @endcode
#define LOG_VERSION_CODE (LOG_VERSION_MAJOR * 10000 + LOG_VERSION_MINOR * 100 + LOG_VERSION_PATCH)

/// @}
