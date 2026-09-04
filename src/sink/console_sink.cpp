#include"logger/sink/console_sink.h"
#include<sstream>
#include<fstream>

ConsoleSink::ConsoleSink(LogLevel errlevel):errlevel_{errlevel}{}

//打印日志,输入格式化后的信息
void ConsoleSink::log(const FormatResult& result){
    if(result.level<errlevel_){
        std::cout<<result;
    }
    else{
        std::cerr<<result;
    }
}

//刷新日志缓冲区
void ConsoleSink::flush(){
    std::cout<<std::flush;
    std::cerr<<std::flush;
}