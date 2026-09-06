#pragma once
#include "logger/formatter.h"
#include "logger/literals.h"

// 日志输出槽
class LogSink {
 public:
  // 打印日志,输入格式化后的信息
  virtual void log(const FormatResult& result) = 0;
  // 刷新日志缓冲区
  virtual void flush() = 0;

  virtual ~LogSink() = default;
};

// //单文件日志输出槽
// class SingleFileSink:LogSink{
// public:
//     SingleFileSink(const std::string& file_path){
//         ofs_.open(file_path,std::ios::out|std::ios::app);
//         if(!ofs_.is_open()){
//             std::cerr<<"SingleFileSink::SingleFileSink: open error"<<std::endl;
//             exit(0);
//         }
//     }

//     //打印日志,输入格式化后的信息
//     void log(const std::string& formatted_msg){
//         ofs_<<formatted_msg;
//     }

//     //刷新日志缓冲区
//     void flush(){
//         ofs_.flush();
//     }
// private:
//     std::ofstream ofs_;//日志文件流
// };

// //多文件日志输出槽(指定目录,槽会自动根据日期和大小新增文件)
// //每隔date_interval_h(默认24)小时新增日志文件,期间如果文件大小超过max_file_size(默认10_mb)也新增日志文件(不会重置date_interval_h)
// class MultiFileSink:LogSink{
// public:
//     MultiFileSink(const std::string& dir,unsigned int date_interval_h=24,unsigned long long
//     max_file_size=10 * 1024 * 1024);

//     //打印日志,输入格式化后的信息
//     void log(const std::string& formatted_msg){
//         check();
//         ofs_<<formatted_msg;
//     }

//     //刷新日志缓冲区
//     void flush(){
//         ofs_.flush();
//     }
// private:
//     //检查是否需要新建日志文件,如果需要,自动创建
//     void check();

//     //更换日志文件,change_date表示是因为日期更新还是尺寸更新,默认因为日期更新
//     void change_log_file(bool change_date=true);

//     std::filesystem::path dir_;//日志文件夹路径
//     std::filesystem::path file_;//当前日志文件路径
//     std::ofstream ofs_;//日志文件流
//     unsigned int date_interval_h_;//日志文件更换间隔
//     unsigned long long max_file_size_;//日志文件最大尺寸
//     int n_;//日志文件同间隔超尺寸计数
//     std::chrono::system_clock::time_point pre_time;//上次创建日志文件的时间
// };
