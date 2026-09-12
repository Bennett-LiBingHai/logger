#include "logger/crash_handler.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#if defined(__linux__)
#include <execinfo.h>
#include <link.h>  // dl_iterate_phdr：取主程序 load bias
#include <sys/syscall.h>
#include <sys/ucontext.h>
#define LOGGER_HAS_CRASH_SUPPORT 1
#endif

namespace {

#ifdef LOGGER_HAS_CRASH_SUPPORT

constexpr int kMaxFrames = 64;
constexpr std::size_t kBufSize = 8192;
constexpr std::size_t kAltStackSize = 64 * 1024;

// 崩溃处理器可接管的信号
constexpr int kSignals[] = {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL};
constexpr std::size_t kSignalCount = sizeof(kSignals) / sizeof(kSignals[0]);

constexpr std::size_t kMaxModules = 64;

int g_fd = -1;                           // 预先打开的崩溃日志 fd
char g_alt_stack[kAltStackSize];         // 独立信号栈（静态存储，handler 才敢用）
volatile sig_atomic_t g_in_handler = 0;  // 递归崩溃防护

// 只接管原本就是默认动作的信号，所以卸载一律恢复 SIG_DFL，无需保存旧 handler。
// 这里只需要记住「哪些信号真的被我们接管了」，避免卸载时误动别人的 handler。
bool g_installed[kSignalCount] = {};

// 已装载模块表，安装时填好，handler 里只读（不能分配、不能 dlopen）
struct Module {
  std::uintptr_t base;  // dlpi_addr 即该模块的 load bias
  std::uintptr_t end;   // base + 最高段的 p_vaddr + p_memsz
  const char* name;     // 模块路径；主程序用 /proc/self/exe 解析结果
};
Module g_modules[kMaxModules];
std::size_t g_module_count = 0;
char g_exe_path[512];  // 主程序路径（dlpi_name 对主程序为空）

// ===== async-signal-safe 的缓冲写入：不分配、不调用 libc 格式化 =====
class BufWriter {
 public:
  BufWriter(char* buf, std::size_t cap) : buf_(buf), cap_(cap) {}

  void ch(char c) {
    if (len_ + 1 < cap_)
      buf_[len_++] = c;
  }

  void str(const char* s) {
    while (*s != '\0')
      ch(*s++);
  }

  void dec(long long v) {
    if (v < 0) {
      ch('-');
      v = -v;
    }
    char tmp[24];
    int n = 0;
    do {
      tmp[n++] = static_cast<char>('0' + (v % 10));
      v /= 10;
    } while (v != 0);
    while (n > 0)
      ch(tmp[--n]);
  }

  void hex(std::uintptr_t v) {
    str("0x");
    char tmp[2 * sizeof(std::uintptr_t)];
    int n = 0;
    do {
      const auto d = static_cast<unsigned>(v & 0xFU);
      tmp[n++] = static_cast<char>(d < 10 ? ('0' + d) : ('a' + d - 10));
      v >>= 4;
    } while (v != 0);
    while (n > 0)
      ch(tmp[--n]);
  }

  [[nodiscard]] const char* data() const {
    return buf_;
  }
  [[nodiscard]] std::size_t size() const {
    return len_;
  }

 private:
  char* buf_;
  std::size_t cap_;
  std::size_t len_ = 0;
};

// write() 可能短写或被信号打断，必须循环
void write_all(int fd, const char* p, std::size_t n) {
  while (n > 0) {
    const ssize_t w = ::write(fd, p, n);
    if (w > 0) {
      p += w;
      n -= static_cast<std::size_t>(w);
      continue;
    }
    if (w < 0 && errno == EINTR)
      continue;
    return;  // 写不进去也没别的办法，不能在这里做任何补救
  }
}

const char* signal_name(int sig) {
  switch (sig) {
  case SIGSEGV:
    return "SIGSEGV";
  case SIGABRT:
    return "SIGABRT";
  case SIGBUS:
    return "SIGBUS";
  case SIGFPE:
    return "SIGFPE";
  case SIGILL:
    return "SIGILL";
  default:
    return "SIG?";
  }
}

// handler 里不能用 gettid()（非 async-signal-safe 封装），用原始系统调用
long current_tid() {
#if defined(SYS_gettid)
  return static_cast<long>(::syscall(SYS_gettid));
#else
  return static_cast<long>(::getpid());
#endif
}

// 填充模块表：每个模块记下 load bias、地址上界与路径。
// dlpi_addr 就是该模块的 load bias（PIE 下为装载基址，非 PIE 下为 0），
// 所以「运行时地址 - dlpi_addr」对主程序和共享库同样成立，无需判断是否 PIE。
int collect_module(dl_phdr_info* info, std::size_t /*size*/, void* /*data*/) {
  if (g_module_count >= kMaxModules)
    return 1;  // 表满即停

  std::uintptr_t highest = 0;
  for (int i = 0; i < info->dlpi_phnum; ++i) {
    const ElfW(Phdr)& ph = info->dlpi_phdr[i];
    if (ph.p_type != PT_LOAD)
      continue;
    const std::uintptr_t seg_end = static_cast<std::uintptr_t>(ph.p_vaddr) + ph.p_memsz;
    if (seg_end > highest)
      highest = seg_end;
  }

  const std::uintptr_t bias = static_cast<std::uintptr_t>(info->dlpi_addr);
  // 主程序的 dlpi_name 为空串，用 /proc/self/exe 的解析结果代替
  const char* name = (info->dlpi_name != nullptr && info->dlpi_name[0] != '\0')
                         ? info->dlpi_name
                         : (g_exe_path[0] != '\0' ? g_exe_path : "<main>");

  g_modules[g_module_count++] = Module{bias, bias + highest, name};
  return 0;
}

// 安装阶段（非 handler）提示：该信号不归我们管，说明原因
void warn_skipped(int sig) {
  char buf[192];
  const int n = std::snprintf(buf, sizeof(buf),
                              "[logger] crash handler: %s 已有其它 handler（sanitizer / 运行时）"
                              "或被显式忽略，不接管——崩溃现场由现有处理负责\n",
                              signal_name(sig));
  if (n > 0)
    ::write(STDERR_FILENO, buf, static_cast<std::size_t>(n));
}

// 运行时地址 → addr2line 的输入；返回所属模块路径，找不到模块返回 nullptr
const char* module_offset(std::uintptr_t addr, std::uintptr_t& offset) {
  for (std::size_t i = 0; i < g_module_count; ++i) {
    if (addr >= g_modules[i].base && addr < g_modules[i].end) {
      offset = addr - g_modules[i].base;
      return g_modules[i].name;
    }
  }
  offset = addr;
  return nullptr;
}

// 信号处理函数
void crash_handler(int sig, siginfo_t* info, void* ucontext) {
  // handler 内再次崩溃：不再尝试记录，直接走默认动作（保留 core dump）
  if (g_in_handler != 0) {
    ::signal(sig, SIG_DFL);
    ::raise(sig);
    return;
  }
  g_in_handler = 1;

  const int fd = (g_fd >= 0) ? g_fd : STDERR_FILENO;

  char buf[kBufSize];
  BufWriter w(buf, sizeof(buf));

  w.str("\n=== CRASH ===\npid=");
  w.dec(static_cast<long>(::getpid()));
  w.str(" tid=");
  w.dec(current_tid());
  w.str("\nsignal=");
  w.str(signal_name(sig));
  w.str("(");
  w.dec(sig);
  w.str(")");

  if (info != nullptr) {
    // 信号产生原因
    w.str(" code=");
    w.dec(info->si_code);
    // 仅 SIGSEGV/SIGBUS 的 si_addr 是"被访问的地址"，其它信号无意义
    if (sig == SIGSEGV || sig == SIGBUS) {
      w.str(" fault_addr=");
      w.hex(reinterpret_cast<std::uintptr_t>(info->si_addr));
    }
  }

  // 精确崩溃点：ucontext 里的 PC 才是出错指令，backtrace 的前几帧是
  // handler 与信号跳板，不能代表崩溃位置
  std::uintptr_t pc = 0;
  std::uintptr_t sp = 0;
  std::uintptr_t bp = 0;
#if defined(__x86_64__)
  if (ucontext != nullptr) {
    const auto* uc = static_cast<const ucontext_t*>(ucontext);
    // uc->uc_mcontext,寄存器现场
    pc = static_cast<std::uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]);
    sp = static_cast<std::uintptr_t>(uc->uc_mcontext.gregs[REG_RSP]);
    bp = static_cast<std::uintptr_t>(uc->uc_mcontext.gregs[REG_RBP]);
  }
#else
  (void)ucontext;
#endif
  // 追加 "  模块路径+偏移"：偏移是该模块的链接期地址，可直接喂 addr2line
  const auto append_location = [&w](std::uintptr_t addr) {
    std::uintptr_t off = 0;
    const char* mod = module_offset(addr, off);
    w.str("  ");
    if (mod != nullptr) {
      w.str(mod);
      w.str("+");
    } else {
      w.str("<unknown module>+");
    }
    w.hex(off);
  };

  w.str("\nfault_pc=");
  w.hex(pc);
  append_location(pc);
  w.str("\n  rsp=");
  w.hex(sp);
  w.str(" rbp=");
  w.hex(bp);

  w.str("\nstack:\n");
  void* frames[kMaxFrames];
  const int n = ::backtrace(frames, kMaxFrames);
  for (int i = 0; i < n; ++i) {
    const auto addr = reinterpret_cast<std::uintptr_t>(frames[i]);
    w.str("  #");
    w.dec(i);
    w.str(" ");
    w.hex(addr);
    append_location(addr);
    w.str("\n");
  }
  w.str("=== END CRASH ===\n");

  write_all(fd, w.data(), w.size());

  // 恢复默认处理并重新抛出：保留 core dump，让 gdb 仍可事后调试
  ::signal(sig, SIG_DFL);
  sigset_t mask;
  ::sigemptyset(&mask);
  ::sigaddset(&mask, sig);
  ::sigprocmask(SIG_UNBLOCK, &mask, nullptr);  // 解除对该信号的屏蔽
  ::raise(sig);

  _exit(128 + sig);  // raise 未生效时的兜底；用 _exit 避免 atexit/静态析构死锁
}

#endif  // LOGGER_HAS_CRASH_SUPPORT

}  // namespace

bool install_crash_handler(const std::string& path) {
#ifdef LOGGER_HAS_CRASH_SUPPORT
  // 输出目标：空路径走 stderr，否则预先打开（handler 里不能 open）
  int fd = -1;
  if (!path.empty()) {
    fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0)
      return false;
  }

  // 主程序路径：dlpi_name 对主程序为空串，这里解析一次供模块表使用
  const ssize_t exe_len = ::readlink("/proc/self/exe", g_exe_path, sizeof(g_exe_path) - 1);
  g_exe_path[exe_len > 0 ? static_cast<std::size_t>(exe_len) : 0] = '\0';

  // 快照已装载模块：handler 里不能 dlopen/分配，只能在安装时取好
  g_module_count = 0;
  ::dl_iterate_phdr(collect_module, nullptr);

  // 热身：backtrace 属于 libgcc，首次调用会触发动态装载（可能 malloc），
  // 必须在 handler 之外先调用一次
  void* warmup[1];
  (void)::backtrace(warmup, 1);

  // 信号单独分配栈
  stack_t ss{};
  ss.ss_sp = g_alt_stack;
  ss.ss_size = sizeof(g_alt_stack);
  if (::sigaltstack(&ss, nullptr) != 0)
    return false;

  // 安装信号处理函数
  struct sigaction sa {};
  sa.sa_sigaction = crash_handler;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  ::sigemptyset(&sa.sa_mask);

  std::size_t installed = 0;
  for (std::size_t i = 0; i < kSignalCount; ++i) {
    const int sig = kSignals[i];

    struct sigaction prev {};
    if (::sigaction(sig, nullptr, &prev) != 0)
      continue;

    // 只接管原本就是默认动作的信号：
    //   - 非默认 handler（sanitizer、JVM 等）可能承担功能职责——JVM 用 SIGSEGV
    //     实现隐式空指针检查，正常的 null 解引用靠它修正 PC 后恢复执行。抢过来会让
    //     正常流程崩溃，且每次 null 检查都写一遍假崩溃日志。
    //   - SIG_IGN 是程序有意忽略该信号，接管会把"忽略"变成"崩溃退出"。
    if (prev.sa_handler != SIG_DFL) {
      warn_skipped(sig);
      continue;
    }

    if (::sigaction(sig, &sa, nullptr) != 0)
      continue;  // 单个信号失败不影响其它信号

    g_installed[i] = true;
    ++installed;
  }

  if (installed == 0) {
    // 一个都没接管：不留半套配置，明确返回失败
    if (fd >= 0)
      ::close(fd);
    return false;
  }

  if (g_fd >= 0 && g_fd != fd)
    ::close(g_fd);  // 重复安装：关掉上一处输出目标

  g_fd = fd;
  g_in_handler = 0;
  return true;
#else
  (void)path;
  return false;
#endif
}

void uninstall_crash_handler() {
#ifdef LOGGER_HAS_CRASH_SUPPORT
  // 只还原我们真正接管过的信号——没接管的别去动，那是别人的 handler
  for (std::size_t i = 0; i < kSignalCount; ++i) {
    if (!g_installed[i])
      continue;
    ::signal(kSignals[i], SIG_DFL);  // 接管时它一定是 SIG_DFL
    g_installed[i] = false;
  }

  if (g_fd >= 0) {
    ::close(g_fd);
    g_fd = -1;
  }
  g_in_handler = 0;
#endif
}
