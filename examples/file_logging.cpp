#include <logger/logger.h>
#include <logger/sink/file_sink.h>
#include <memory>
#include <string>

using namespace logger;  // 库的公共符号都在 logger:: 下

// 文件输出与轮转：按大小切分，最多保留 max_backups 个文件
int main() {
  FileSinkConfig sink_cfg;
  sink_cfg.dir = "/tmp/logger_example";  // 目录不存在会自动创建，创建失败则写入失败
  sink_cfg.date_interval_h = 24;         // 每 24 小时按日期切一个
  sink_cfg.max_file_size = 1024;         // 单文件 1KB（示例用，生产按需调大）
  sink_cfg.max_backups = 5;              // 掉多余的旧文件

  Logger::get_instance().add_sink(std::make_shared<FileSink>(sink_cfg));

  for (int i = 0; i < 200; ++i)
    LOG_INFO("entry {} payload={}", i, std::string(64, 'x'));

  Logger::get_instance().flush_all();

  // 运行后看 /tmp/logger_example/ 下切分出的文件：
  //   ls -l /tmp/logger_example/
  return 0;
}
