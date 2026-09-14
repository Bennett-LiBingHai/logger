#pragma once
#include <chrono>
#include <string>
#include <string_view>
#include <thread>

#include "logger/config.h"

/// @file detail/utils.h
/// @brief 时间格式化、文本/JSON 转义、UTF-8 安全截断与线程 id 缓存。
///
/// 内部工具集，不承诺接口稳定；用户不应直接包含本文件。

/// @brief 线程安全、跨平台的 time_t → 本地时间转换。
/// @param t 时间戳。
/// @param tm 输出参数，转换结果。
/// @return 是否转换成功。
bool localtime_safe(time_t t, std::tm& tm);

/// @brief 线程安全、跨平台的 time_t → UTC 时间转换。
/// @param t 时间戳。
/// @param tm 输出参数，转换结果。
/// @return 是否转换成功。
bool gmtime_safe(time_t t, std::tm& tm);

/// @brief 格式化日期时间，按配置选择本地时间或 UTC。
/// @param tp 时间点。
/// @param config 配置（决定时区与格式）。
/// @return 格式化后的时间串。
std::string format_time(const std::chrono::system_clock::time_point& tp, const LogConfig& config);

/// @brief 格式化日期时间，含毫秒：`YYYY-MM-DDTHH:MM:SS.mmm`。
/// @param tp 时间点。
/// @param config 配置（决定时区）。
/// @return 格式化后的时间串。
/// @note 文本与 JSON 统一走这里，不要在各自 Formatter 里单独拼时间。
std::string format_time_ms(const std::chrono::system_clock::time_point& tp,
                           const LogConfig& config);

/// @brief 线程 id 的文本形式，按 id 缓存。
///
/// `std::thread::id` 只能经 `operator<<` 输出，而构造一个 `ostringstream` 的代价
/// 远高于这里要拼的那几个字符（实测每条记录约 480 ns）。线程 id 不会变，
/// 缓存起来即可。
///
/// @param id 线程 id。
/// @return 该 id 的文本形式；同一 id 重复调用直接返回缓存。
/// @note 缓存**按 id 值命中**而不是"当前线程"：异步模式下格式化发生在后台线程，
///       而记录里存的是业务线程的 id。只有单槽缓存，最坏情况退化成每次重新格式化，
///       不会随线程数增长而占用内存。
const std::string& thread_id_str(std::thread::id id);

/// @brief JSON 字符串转义。
///
/// 处理双引号、反斜杠、退格、换页、制表、回车、换行以及 0x00 ~ 0x1F 控制字符，
/// 保证结果能直接放进 JSON 字符串字面量里。
///
/// @param raw 原始字符串。
/// @return 转义后的字符串（不含外层引号）。
std::string json_escape(const std::string& raw);

/// @brief 文本转义：把反斜杠与控制字符转成字面转义序列。
///
/// 处理反斜杠、退格、换页、制表、回车、换行以及 0x00 ~ 0x1F、0x7F。
/// 反斜杠必须一起转义 —— 否则「数据里本来就是反斜杠+n」与「转义后的换行」无法区分，
/// 转义就不可逆了。
///
/// @param raw 原始字符串。
/// @return 转义后的字符串；保证其中不再含裸换行，即一条日志只占一行。
std::string text_escape(std::string_view raw);

/// @brief 同 text_escape，但结果追加到 out。
/// @param raw 原始字符串。
/// @param out 输出缓冲。
/// @note 无需转义时走快路径直接 append，不产生额外分配。
void append_text_escaped(std::string_view raw, std::string& out);

/// @brief 把字符串 UTF-8 安全地截断到至多 limit 字节。
/// @param s 待截断的字符串，原地修改。
/// @param limit 字节上限；0 表示截断为空。
/// @note 会退到完整码点边界，绝不切出非法 UTF-8；被截断时以 "..." 结尾。
void truncate_utf8(std::string& s, std::size_t limit);
