# 外部消费者冒烟工程：验证安装出来的包能被 find_package 找到并正常链接。
#
# 不在主构建里（顶层 CMakeLists 不会 add_subdirectory 到 test/consumer），
# 由 CI 的 consumer job 和本地手工执行：
#
#   cmake -S . -B build -DBUILD_TESTS=OFF -DBUILD_BENCHMARKS=OFF
#   cmake --build build && cmake --install build --prefix "$PWD/_prefix"
#   cmake -S test/consumer -B /tmp/consumer-build -DCMAKE_PREFIX_PATH="$PWD/_prefix"
#   cmake --build /tmp/consumer-build && /tmp/consumer-build/consumer_app
cmake_minimum_required(VERSION 3.16)
project(logger_consumer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(logger 1.0 REQUIRED)

add_executable(consumer_app main.cpp)
target_link_libraries(consumer_app PRIVATE logger::logger)
