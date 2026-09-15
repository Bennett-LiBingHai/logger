#include <logger/logger.h>
#include <logger/sink/console_sink.h>
#include <memory>
#include <string>

using namespace logger;  // 库的公共符号都在 logger:: 下

// 敏感字段脱敏：默认按关键词整体隐藏，也可自定义规则（如卡号保留前后几位）
int main() {
  Logger::get_instance().add_sink(std::make_shared<ConsoleSink>());

  LogConfig cfg;
  cfg.enable_sensitive_field_mask = true;  // 默认关键词表见 is_sensitive_key()
  Logger::get_instance().set_config(cfg);

  Logger::get_instance().info("login", KV("user", "tom"), KV("password", "s3cret"));
  Logger::get_instance().info("pay", KV("credit_card", "6222021234567890"));

  // 自定义：复用默认关键词表，卡号保留前 6 后 4（PCI 风格）
  cfg.sensitive_field_masker = [](const std::string& key, const std::string& value) {
    if (!is_sensitive_key(key))
      return value;
    if (value.size() <= 10)
      return std::string("***");
    return value.substr(0, 6) + "****" + value.substr(value.size() - 4);
  };
  Logger::get_instance().set_config(cfg);

  Logger::get_instance().info("pay", KV("credit_card", "6222021234567890"), KV("password", "short"),
                              KV("user", "tom"));
  return 0;
}
