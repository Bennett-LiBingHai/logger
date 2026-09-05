// #include"log_sink.h"

// MultiFileSink::MultiFileSink(const std::string& dir,unsigned int date_interval_h,unsigned long
// long max_file_size)
// :dir_{dir},date_interval_h_{date_interval_h},max_file_size_{max_file_size},n_(0)
// {
//     if(!std::filesystem::is_directory(dir_)){
//         std::cerr<<"MultiFileSink::MultiFileSink: "<<dir<<" is not a directory"<<std::endl;
//         exit(0);
//     }
//     change_log_file();
// }

// //更换日志文件,change_date表示是因为日期更新还是尺寸更新,默认因为日期更新
// void MultiFileSink::change_log_file(bool change_date){
//     ofs_.close();
//     if(ofs_.is_open()){
//         std::cerr<<"MultiFileSink::change_log_file: close error";
//         exit(0);
//     }

//     char buf[64];
//     if(change_date){
//         time_t t=std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
//         tm daytime=*localtime(&t);
//         if(strftime(buf,sizeof(buf),"%Y_%m_%d_%H.log.",&daytime)==0){
//             std::cerr<<"MultiFileSink::MultiFileSink: strftime error: filename too
//             long"<<std::endl; exit(0);
//         }
//         file_=dir_/(buf+std::to_string(n_));
//     }
//     else{
//         ++n_;
//         auto end = file_.c_str();
//         char* p=buf;
//         char* cur=p;
//         while(*end!='\0'){
//             *p=*end++;
//             if(*p=='.')cur=p;
//             ++p;
//         }
//         if(*cur!='.')std::cerr<<"MultiFileSink::change_log_file: file_ not contains
//         '.'"<<std::endl; std::string n{std::to_string(n_)}; for(auto& c:n){
//             *(++cur)=c;
//         }
//         *cur='\0';
//         file_=dir_/buf;
//     }

//     ofs_.open(file_,std::ios::out|std::ios::app);
//     if(!ofs_.is_open()){
//         std::cerr<<"MultiFileSink::MultiFileSink: open error"<<std::endl;
//         exit(0);
//     }
//     pre_time=std::chrono::system_clock::now();
// }

// //检查是否需要新建日志文件,如果需要,自动创建
// void MultiFileSink::check(){
//     auto
//     interval=std::chrono::duration_cast<std::chrono::hours>(std::chrono::system_clock::now()-pre_time);
//     if(interval.count()>=date_interval_h_){
//         change_log_file();
//         return;
//     }
//     if(!std::filesystem::exists(file_)){
//         std::cerr<<"MultiFileSink::check: "<<file_<<" is not exists"<<std::endl;
//         exit(0);
//     }
//     if(std::filesystem::file_size(file_)>=max_file_size_){
//         change_log_file(false);
//     }
// }
