#pragma once
#include"logger/sink.h"
#include"logger/level.h"

//控制台日志输出槽
class ConsoleSink:public LogSink{
public:
    ConsoleSink(LogLevel errlevel=LogLevel::ERROR);

    //打印日志,输入格式化后的信息
    void log(const FormatResult& result) override;

    //刷新日志缓冲区
    void flush() override;
private:
    LogLevel errlevel_;//输出到stderr的最低级别
};