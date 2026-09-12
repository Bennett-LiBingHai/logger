#pragma once
#include <cstddef>
#include <string>
#include <vector>

// 堆栈追踪采集（M5：堆栈信息）
//
// 采集与符号化分离：
//   - capture() 只调用 backtrace() 拿返回地址列表（快、不分配）
//   - str() 才把地址符号化（dladdr + demangle，慢），结果缓存
// 异步模式下符号化天然发生在后台写入线程，业务线程只付采集开销；
// 同步模式下 str() 在格式化时被调用一次，之后走缓存。
//
// 显式请求堆栈就是普通字段：KV("stacktrace", StackTrace::capture())
//
// 注意：本类型不承诺线程安全（同一对象同一时刻只应由一个线程编码），但可拷贝/移动，以便随 Record
// 入异步队列。
class StackTrace {
 public:
  StackTrace() = default;

  // 采集当前调用栈：
  //   skip       从栈顶丢弃的帧数（含 capture 自身），默认 1
  //   depth      最大帧数
  //   max_length 渲染后字节上限（0 = 不限）。超出时按帧丢弃并附 "... (+N frames)"，
  //              保证堆栈不霸占整条日志的 max_log_item_size、也不被字节级硬切。
  // 平台不支持时返回空对象（empty() == true）。
  [[nodiscard]] static StackTrace capture(std::size_t skip = 1, std::size_t depth = 10,
                                          std::size_t max_length = 512);

  // 惰性符号化并缓存（受 max_length 约束）；无帧时返回空串
  [[nodiscard]] const std::string& str() const;

  [[nodiscard]] bool empty() const {
    return frames_.empty();
  }

 private:
  std::vector<void*> frames_;   // 返回地址（未符号化）
  std::size_t max_length_ = 0;  // 渲染字节上限，0 = 不限
  mutable std::string cached_;  // 符号化结果缓存
  mutable bool symbolized_ = false;
};
