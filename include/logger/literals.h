#pragma once
#include<iostream>

//logger字面量
namespace logger::literals{
    //byte
    constexpr unsigned long long operator""_b(unsigned long long n){
        return n;
    }
    //kb
    constexpr unsigned long long operator""_kb(unsigned long long n){
        return n*1024ULL;
    }
    //mb
    constexpr unsigned long long operator""_mb(unsigned long long n){
        return n*1024ULL*1024ULL;
    }
    //gb
    constexpr unsigned long long operator""_gb(unsigned long long n){
        return n*1024ULL*1024ULL*1024ULL;
    }
}