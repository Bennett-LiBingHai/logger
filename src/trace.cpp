#include "logger/trace.h"

#include <cctype>
#include <chrono>
#include <cstddef>
#include <random>

namespace {

// traceparent 各段字符数
constexpr std::size_t kVersionLen = 2;
constexpr std::size_t kTraceIdLen = 32;
constexpr std::size_t kSpanIdLen = 16;
constexpr std::size_t kFlagsLen = 2;

// hex 字符 → 数值；非法返回 -1
int hex_value(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  const char l = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (l >= 'a' && l <= 'f')
    return l - 'a' + 10;
  return -1;
}

// 长度与字符集校验（大小写 hex 都接受）
bool is_hex(std::string_view s, std::size_t n) {
  if (s.size() != n)
    return false;
  for (char c : s)
    if (hex_value(c) < 0)
      return false;
  return true;
}

bool is_all_zero(std::string_view s) {
  for (char c : s)
    if (c != '0')
      return false;
  return true;
}

// 每线程一个生成器；random_device 不可用时退化为时钟熵（关联 id 不要求密码学强度）
std::mt19937_64 make_rng() {
  try {
    std::random_device rd;
    std::seed_seq seq{rd(), rd(), rd(), rd()};
    return std::mt19937_64{seq};
  } catch (...) {
    const auto ticks = static_cast<unsigned long long>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::seed_seq seq{static_cast<unsigned>(ticks), static_cast<unsigned>(ticks >> 32)};
    return std::mt19937_64{seq};
  }
}

std::mt19937_64& rng() {
  thread_local std::mt19937_64 gen = make_rng();
  return gen;
}

// 生成 n 个小写 hex 字符（n 为偶数）
std::string random_hex(std::size_t n) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string s(n, '0');
  for (std::size_t i = 0; i + 1 < n; i += 2) {
    const auto byte = static_cast<unsigned>(rng()() & 0xFFu);
    s[i] = kDigits[byte >> 4];
    s[i + 1] = kDigits[byte & 0x0Fu];
  }
  return s;
}

}  // namespace

bool TraceContext::sampled() const {
  if (!is_hex(trace_flags, kFlagsLen))
    return false;  // 格式非法按未采样处理
  // trace_flags 是 8 位标志位，只有 bit0 是 sampled，即最后一个 hex 字符的最低位
  return (hex_value(trace_flags[1]) & 1) != 0;
}

TraceContext generate_trace() {
  TraceContext ctx;  // version / trace_flags 由结构体默认值提供
  do {
    ctx.trace_id = random_hex(kTraceIdLen);
  } while (is_all_zero(ctx.trace_id));  // W3C 规定不得为全 0
  do {
    ctx.span_id = random_hex(kSpanIdLen);
  } while (is_all_zero(ctx.span_id));
  return ctx;
}

bool parse_traceparent(std::string_view header, TraceContext& out) {
  // 至少 4 段：version-trace_id-span_id-trace_flags；更高版本可能追加字段
  std::string_view part[4];
  std::size_t begin = 0;
  std::size_t end = std::string_view::npos;  // 循环结束后：第 4 段之后的分隔符（若有追加字段）
  for (int i = 0; i < 4; ++i) {
    end = header.find('-', begin);
    if (i < 3) {
      if (end == std::string_view::npos)
        return false;
      part[i] = header.substr(begin, end - begin);
      begin = end + 1;  // 第 4 段不推进 begin，end 即为其后的分隔符
    } else {
      part[i] = (end == std::string_view::npos) ? header.substr(begin)
                                                : header.substr(begin, end - begin);
    }
  }

  if (!is_hex(part[0], kVersionLen))
    return false;
  // version 00 不得有追加字段；更高版本的追加字段按规范忽略
  if (part[0] == "00" && end != std::string_view::npos)
    return false;

  if (!is_hex(part[1], kTraceIdLen) || is_all_zero(part[1]))
    return false;
  if (!is_hex(part[2], kSpanIdLen) || is_all_zero(part[2]))
    return false;
  if (!is_hex(part[3], kFlagsLen))
    return false;

  out.version.assign(part[0]);
  out.trace_id.assign(part[1]);
  out.span_id.assign(part[2]);
  out.trace_flags.assign(part[3]);
  return true;
}

std::string make_traceparent(const TraceContext& ctx) {
  // 四个字段一视同仁：任一非法即返回空串，不做默认值替换
  if (!is_hex(ctx.version, kVersionLen))
    return {};
  if (!is_hex(ctx.trace_id, kTraceIdLen) || is_all_zero(ctx.trace_id))
    return {};
  if (!is_hex(ctx.span_id, kSpanIdLen) || is_all_zero(ctx.span_id))
    return {};
  if (!is_hex(ctx.trace_flags, kFlagsLen))
    return {};

  std::string out;
  out.reserve(kVersionLen + kTraceIdLen + kSpanIdLen + kFlagsLen + 3);
  out += ctx.version;
  out += '-';
  out += ctx.trace_id;
  out += '-';
  out += ctx.span_id;
  out += '-';
  out += ctx.trace_flags;
  return out;
}
