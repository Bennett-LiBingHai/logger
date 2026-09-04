#pragma once
#include"logger/sink.h"
#include"logger/config.h"
#include"logger/record.h"
#include"logger/formatter/text_formatter.h"
#include<vector>
#include<memory>
#include<cstdio>
#include<mutex>

//日志器
class Logger{
public:

    //获取实例
    [[nodiscard]] static Logger& get_instance();

    //打印日志带格式
    template<typename... Args>
    void log(LogLevel logLevel,const char* file,int line,const char* func,const char* fmt,Args&&... args);

    //打印日志纯字符串
    void log(LogLevel logLevel,const char* file,int line,const char* func,const char* str);

    //增加输出槽
    void add_sink(std::shared_ptr<LogSink> log_sink);

    //更新配置
    void set_config(const LogConfig& config);

    //获取配置
    LogConfig get_config();

    //刷新所有日志缓冲区
    void flush_all();

    ~Logger()=default;
private:
    Logger()=default;
    Logger(const Logger&)=delete;
    Logger& operator=(const Logger&)=delete;
    Logger(Logger&&)=delete;
    Logger& operator=(Logger&&)=delete;

    std::vector<std::shared_ptr<LogSink>> sinks_;//输出槽数组
    LogConfig config_;//日志配置
    std::mutex mtx_;//锁
};

//获取文件名
constexpr const char* filename_of(const char* path){
    const char* cur=path;
    while(*cur)++cur;
    while(cur!=path){
        if(*cur=='/'||*cur=='\\')return cur+1;
        --cur;
    }
    return path;
}

//宏封装
#define __FILENAME__ filename_of(__FILE__) //获取文件名
#define LOG_TRACE(fmt , ...)  Logger::get_instance().log(LogLevel::TRACE,__FILENAME__,__LINE__,__func__,fmt,##__VA_ARGS__) //细粒度调试
#define LOG_DEBUG(fmt , ...)  Logger::get_instance().log(LogLevel::DEBUG,__FILENAME__,__LINE__,__func__,fmt,##__VA_ARGS__) //调试
#define LOG_INFO(fmt , ...)  Logger::get_instance().log(LogLevel::INFO,__FILENAME__,__LINE__,__func__,fmt,##__VA_ARGS__) //信息
#define LOG_WARN(fmt , ...)  Logger::get_instance().log(LogLevel::WARN,__FILENAME__,__LINE__,__func__,fmt,##__VA_ARGS__) //警告
#define LOG_ERROR(fmt , ...)  Logger::get_instance().log(LogLevel::ERROR,__FILENAME__,__LINE__,__func__,fmt,##__VA_ARGS__) //错误
#define LOG_FATAL(fmt , ...)  Logger::get_instance().log(LogLevel::FATAL,__FILENAME__,__LINE__,__func__,fmt,##__VA_ARGS__) //致命

//打印日志带格式
template<typename... Args>
void Logger::log(LogLevel logLevel,const char* file,int line,const char* func,const char* fmt,Args&&... args){
    std::unique_lock<std::mutex> lock(mtx_);
    if(logLevel<config_.log_level)return;
    char buf[config_.max_log_item_size+1];
    snprintf(buf,sizeof(buf),fmt,std::forward<Args>(args)...);
    Record msg{
        std::chrono::system_clock::now(),
        logLevel,
        buf,
        std::this_thread::get_id(),
        file,line,func
    };
    FormatResult result = TextFormatter::format(msg,config_);
    //超过长度自动截断
    if(result.formatted_msg.size()>config_.max_log_item_size){
        result.formatted_msg.resize(config_.max_log_item_size);
    }
    for(auto& sink:sinks_){
        sink->log(result);
    }
}