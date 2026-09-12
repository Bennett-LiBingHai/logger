#include "logger/stacktrace.h"

#include <cstdio>

#include "logger/error.h"   // demangle
#include "logger/utiils.h"  // json_escape

#if defined(__GLIBC__)
#include <dlfcn.h>
#include <execinfo.h>
#define LOGGER_HAS_BACKTRACE 1

namespace {

// 符号化一帧：函数名（demangle 后）+ 模块内偏移（便于离线 addr2line）
std::string symbolize_frame(void* addr) {
  Dl_info info{};
  if (!::dladdr(addr, &info))
    return "???";
  if (info.dli_sname == nullptr)  // 无符号名是常态（如 stripped 的 libc、静态函数）
    return info.dli_fname ? info.dli_fname : "???";

  std::string frame = demangle(info.dli_sname);
  char buf[320];
  const auto off = static_cast<const char*>(addr) - static_cast<const char*>(info.dli_saddr);
  std::snprintf(buf, sizeof(buf), " (%s+0x%zx)", info.dli_fname ? info.dli_fname : "?",
                static_cast<std::size_t>(off));
  return frame + buf;
}

}  // namespace
#endif

StackTrace StackTrace::capture(std::size_t skip, std::size_t depth, std::size_t max_length) {
  StackTrace st;
  st.max_length_ = max_length;
#ifdef LOGGER_HAS_BACKTRACE
  if (depth == 0)
    return st;

  st.frames_.resize(depth);
  const int n = ::backtrace(st.frames_.data(), static_cast<int>(depth));
  if (n <= static_cast<int>(skip)) {
    st.frames_.clear();
    return st;
  }
  st.frames_.erase(st.frames_.begin(), st.frames_.begin() + static_cast<std::ptrdiff_t>(skip));
  st.frames_.resize(static_cast<std::size_t>(n) - skip);
#else
  (void)skip;
  (void)depth;
#endif
  return st;
}

const std::string& StackTrace::str() const {
  if (symbolized_)
    return cached_;
  symbolized_ = true;

#ifdef LOGGER_HAS_BACKTRACE
  // 预留给 "... (+N frames)\n"，保证截断后的结果仍在预算内
  constexpr std::size_t kSuffixReserve = 32;

  char buf[320];
  std::size_t emitted = 0;
  for (void* addr : frames_) {
    std::snprintf(buf, sizeof(buf), "#%zu ", emitted);
    std::string line = buf;
    line += symbolize_frame(addr);
    line += '\n';

    // 按帧截断：宁可少输出几帧，也不让整条日志超预算被字节级硬切
    if (max_length_ != 0 && cached_.size() + line.size() + kSuffixReserve > max_length_)
      break;

    cached_ += line;
    ++emitted;
  }

  if (emitted < frames_.size()) {
    std::snprintf(buf, sizeof(buf), "... (+%zu frames)\n", frames_.size() - emitted);
    cached_ += buf;
  }
#endif
  return cached_;
}
