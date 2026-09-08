#include "logger/sink/console_sink.h"

#include <iostream>

ConsoleSink::ConsoleSink(LogLevel errlevel) : errlevel_{errlevel} {}

// 打印日志,输入格式化后的信息
bool ConsoleSink::log(const FormatResult& result) {
  std::ostream& os = (result.level < errlevel_) ? std::cout : std::cerr;
  os << result;
  return !os.fail();
}

// 刷新日志缓冲区
void ConsoleSink::flush() {
  std::cout << std::flush;
  std::cerr << std::flush;
}
