#pragma once
#include <string>

/// @file crash_handler.h
/// @brief 崩溃信号处理：进程崩溃时写下精确现场，随后保留 core dump 语义退出。

/// @brief 安装崩溃处理器。
///
/// 进程收到 SIGSEGV / SIGABRT / SIGBUS / SIGFPE / SIGILL 时，把**精确现场**写到日志：
/// pid / tid、信号名与编号、si_code、故障地址（si_addr）、出错指令指针
/// （ucontext 里的 PC / RSP / RBP）、image base、栈回溯原始地址。
///
/// @param path 崩溃日志的输出路径；传空串则写 stderr。
/// @return 是否**至少接管了一个信号**。若某些信号已被其它库接管（见下），
///         那些信号会被跳过；全部被跳过时返回 false。
///
/// @par 与 StackTrace 的区别（为什么必须是另一套实现）
/// signal handler 里只能调用 async-signal-safe 接口：不能 malloc、不能用 std::string。
/// 而 StackTrace::str() 全程分配（std::string / demangle / snprintf），因此不能进
/// handler；Logger 也不能用 —— 它的 mutex 可能正被崩溃线程持有（死锁），
/// 异步线程可能已经死了。所以这里只用 write() + 自实现的数字格式化 + backtrace()。
///
/// @par 不做符号化
/// 函数名与行号需要 malloc，只能事后离线用 addr2line。输出里的偏移量 =
/// 运行时地址 − 主程序 load bias，正是 addr2line 需要的链接期地址；
/// PIE 与非 PIE 一视同仁（load bias 由 dl_iterate_phdr 的 dlpi_addr 直接给出）。
///
/// @par 行为约定
/// - 写完现场后恢复默认处理并重新抛出信号，**保留 core dump 语义**，便于事后 gdb
/// - 使用独立信号栈（sigaltstack），栈溢出导致的崩溃也能记录
/// - handler 内再次崩溃时直接走默认动作，不递归
///
/// @par 与已有 handler 的关系
/// - 某个信号上**已有非默认 handler**（ASan / TSan、JVM 等）时，该信号**不接管**，
///   并在 stderr 提示一句。这类 handler 可能承担功能职责 —— JVM 用 SIGSEGV 实现
///   隐式空指针检查，正常的 null 解引用靠它修正 PC 后恢复执行；抢过来会让正常流程
///   崩溃，且每次 null 检查都会写一遍假崩溃日志
/// - 卸载时精确还原接管前的 handler，而不是一律恢复 SIG_DFL
bool install_crash_handler(const std::string& path = std::string());

/// @brief 卸载崩溃处理器：把接管过的信号恢复成接管前的处理，并关闭输出文件。
/// @note 只还原真正接管过的信号，不会动其它库安装的 handler。
void uninstall_crash_handler();
