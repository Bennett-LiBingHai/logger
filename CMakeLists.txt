cmake_minimum_required(VERSION 3.16)
project(
    logger
    VERSION 1.0.0
    LANGUAGES CXX C
)

# 生成 compile_commands.json，供 clang-tidy / clangd 使用
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 消毒器开关：thread / address / undefined / leak（逗号分隔可组合，注意 thread 不能与 address/leak 混用）
# 注：仅 thread 需要 sudo sysctl vm.mmap_rnd_bits=28（address/undefined/leak 不需要）
set(LOGGER_SANITIZE "" CACHE STRING "Sanitizer(s) to enable: thread, address, undefined, leak")
if(LOGGER_SANITIZE)
    message(STATUS "Sanitizer enabled: ${LOGGER_SANITIZE}")
    #-fsanitize: 让编译器在生成的机器码里插入检查逻辑
    #-g: 生成调试符号
    #-fno-omit-frame-pointer: 不省略帧指针，让 Sanitizer 或调试器能生成更准确的调用栈
    add_compile_options(-fsanitize=${LOGGER_SANITIZE} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${LOGGER_SANITIZE})#链接对应的 Sanitizer 运行时库
endif()

# 源文件目录结构与 include/ 一一对应；detail/ 下为内部实现
set(SRC_FILES
    ${CMAKE_SOURCE_DIR}/src/crash_handler.cpp
    ${CMAKE_SOURCE_DIR}/src/field.cpp
    ${CMAKE_SOURCE_DIR}/src/logger.cpp
    ${CMAKE_SOURCE_DIR}/src/stacktrace.cpp
    ${CMAKE_SOURCE_DIR}/src/trace.cpp
    ${CMAKE_SOURCE_DIR}/src/detail/utils.cpp
    ${CMAKE_SOURCE_DIR}/src/detail/formatter/text_formatter.cpp
    ${CMAKE_SOURCE_DIR}/src/detail/formatter/json_formatter.cpp
    ${CMAKE_SOURCE_DIR}/src/sink/console_sink.cpp
    ${CMAKE_SOURCE_DIR}/src/sink/file_sink.cpp
    )

add_library(logger STATIC ${SRC_FILES})

# 带命名空间的别名：供 add_subdirectory / FetchContent 的消费者使用，避免与别的项目的
# `logger` 目标撞名。注意 ALIAS 不能被 install/export —— 对 find_package 的消费者，
# 命名空间由下面的 install(EXPORT ... NAMESPACE logger::) 提供
add_library(logger::logger ALIAS logger)

target_compile_features(logger PUBLIC cxx_std_17)

# BUILD_INTERFACE / INSTALL_INTERFACE：源码树里指向 include/，安装后指向 <前缀>/include
include(GNUInstallDirs)
target_include_directories(
    logger PUBLIC $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
                  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

find_package(Threads REQUIRED)
# dladdr 需要 libdl（glibc ≥ 2.34 已并入 libc，CMAKE_DL_LIBS 会自动处理为空）
target_link_libraries(logger PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

# 堆栈符号化依赖动态符号表，-rdynamic 必须作用在最终可执行文件上 → INTERFACE 传递
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_link_options(logger INTERFACE -rdynamic)
endif()

# —— 安装与导出 ——
# 装完之后外部项目可以：
#   find_package(logger 1.0 REQUIRED)
#   target_link_libraries(app PRIVATE logger::logger)
include(CMakePackageConfigHelpers)

install(TARGETS logger EXPORT loggerTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

set(LOGGER_CMAKE_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/logger)
install(EXPORT loggerTargets
        NAMESPACE logger::
        FILE loggerTargets.cmake
        DESTINATION ${LOGGER_CMAKE_DIR})

configure_package_config_file(
    ${CMAKE_SOURCE_DIR}/cmake/loggerConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/loggerConfig.cmake
    INSTALL_DESTINATION ${LOGGER_CMAKE_DIR})
# find_package时可以指定版本号,需要版本号与VERSION兼容,SameMajorVersion表示大版本相同时兼容
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/loggerConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/loggerConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/loggerConfigVersion.cmake
        DESTINATION ${LOGGER_CMAKE_DIR})

# ——————————————————————————————————————————————————————————————————————
# 可运行示例
add_executable(logger_example ${CMAKE_SOURCE_DIR}/examples/basic_usage.cpp)
target_link_libraries(logger_example PRIVATE logger::logger)
target_compile_features(logger_example PRIVATE cxx_std_17)

foreach(example file_logging async_logging trace_logging masking dynamic_level)
    add_executable(example_${example} ${CMAKE_SOURCE_DIR}/examples/${example}.cpp)
    target_link_libraries(example_${example} PRIVATE logger::logger)
    target_compile_features(example_${example} PRIVATE cxx_std_17)
endforeach()


option(BUILD_TESTS "Build tests" ON)
if(BUILD_TESTS)
    enable_testing()
    find_package(GTest REQUIRED)

    set(TEST_SOURCES
        ${CMAKE_SOURCE_DIR}/test/unit/test_level.cpp
        ${CMAKE_SOURCE_DIR}/test/unit/test_version.cpp
        ${CMAKE_SOURCE_DIR}/test/unit/test_config.cpp
        ${CMAKE_SOURCE_DIR}/test/unit/test_formatter.cpp
        ${CMAKE_SOURCE_DIR}/test/unit/test_field.cpp
        ${CMAKE_SOURCE_DIR}/test/unit/test_format.cpp
        ${CMAKE_SOURCE_DIR}/test/unit/test_json_formatter.cpp
        ${CMAKE_SOURCE_DIR}/test/unit/test_file_sink.cpp
        ${CMAKE_SOURCE_DIR}/test/unit/test_logger.cpp
        ${CMAKE_SOURCE_DIR}/test/unit/test_context.cpp
        ${CMAKE_SOURCE_DIR}/test/unit/test_error.cpp
        ${CMAKE_SOURCE_DIR}/test/unit/test_stacktrace.cpp
        ${CMAKE_SOURCE_DIR}/test/unit/test_trace.cpp
        ${CMAKE_SOURCE_DIR}/test/unit/test_mask.cpp
        ${CMAKE_SOURCE_DIR}/test/unit/test_injection.cpp
        ${CMAKE_SOURCE_DIR}/test/unit/test_limits.cpp
        ${CMAKE_SOURCE_DIR}/test/unit/test_stats.cpp
        ${CMAKE_SOURCE_DIR}/test/unit/test_dedup.cpp
        ${CMAKE_SOURCE_DIR}/test/integration/test_concurrency.cpp
    )

    add_executable(test_logger ${TEST_SOURCES})
    target_link_libraries(test_logger PRIVATE logger::logger GTest::gtest GTest::gtest_main)
    target_compile_features(test_logger PRIVATE cxx_std_17)
    target_include_directories(test_logger PRIVATE ${CMAKE_SOURCE_DIR}/test)
    # 把 project(VERSION) 透传进去，让 test_version 校验 version.h 与它不漂移
    target_compile_definitions(test_logger PRIVATE LOGGER_CMAKE_VERSION="${PROJECT_VERSION}")
    add_test(NAME test_logger COMMAND test_logger)

    # 手动观察 ConsoleSink 按级别路由（Error+ → stderr，其余 → stdout），独立二进制避免污染单例
    add_executable(test_console ${CMAKE_SOURCE_DIR}/test/integration/test_console_output.cpp)
    target_link_libraries(test_console PRIVATE logger::logger GTest::gtest GTest::gtest_main)
    target_compile_features(test_console PRIVATE cxx_std_17)
    target_include_directories(test_console PRIVATE ${CMAKE_SOURCE_DIR}/test)
    add_test(NAME test_console COMMAND test_console)

    # 异步日志，独立二进制避免污染单例的同步/异步状态
    add_executable(test_async ${CMAKE_SOURCE_DIR}/test/integration/test_async.cpp)
    target_link_libraries(test_async PRIVATE logger::logger GTest::gtest GTest::gtest_main)
    target_compile_features(test_async PRIVATE cxx_std_17)
    target_include_directories(test_async PRIVATE ${CMAKE_SOURCE_DIR}/test)
    add_test(NAME test_async COMMAND test_async)

    # 异步优雅关闭，独立二进制（close() 会停止异步线程，单例不可复用）
    add_executable(test_async_close ${CMAKE_SOURCE_DIR}/test/integration/test_async_close.cpp)
    target_link_libraries(test_async_close PRIVATE logger::logger GTest::gtest GTest::gtest_main)
    target_compile_features(test_async_close PRIVATE cxx_std_17)
    target_include_directories(test_async_close PRIVATE ${CMAKE_SOURCE_DIR}/test)
    add_test(NAME test_async_close COMMAND test_async_close)

    # 文件系统异常：权限、目录消失、写失败降级、fd 泄漏
    add_executable(test_io_failure ${CMAKE_SOURCE_DIR}/test/integration/test_io_failure.cpp)
    target_link_libraries(test_io_failure PRIVATE logger::logger GTest::gtest GTest::gtest_main)
    target_compile_features(test_io_failure PRIVATE cxx_std_17)
    target_include_directories(test_io_failure PRIVATE ${CMAKE_SOURCE_DIR}/test)
    add_test(NAME test_io_failure COMMAND test_io_failure)

    # 生命周期：并发 close、重复 close、线程回收（独立二进制，close 是终态）
    add_executable(test_lifecycle ${CMAKE_SOURCE_DIR}/test/integration/test_lifecycle.cpp)
    target_link_libraries(test_lifecycle PRIVATE logger::logger GTest::gtest GTest::gtest_main)
    target_compile_features(test_lifecycle PRIVATE cxx_std_17)
    target_include_directories(test_lifecycle PRIVATE ${CMAKE_SOURCE_DIR}/test)
    add_test(NAME test_lifecycle COMMAND test_lifecycle)

    # 持续高并发（固定时长，默认 2 秒；LOGGER_SOAK_SECONDS 可调大）
    add_executable(test_soak ${CMAKE_SOURCE_DIR}/test/integration/test_soak.cpp)
    target_link_libraries(test_soak PRIVATE logger::logger GTest::gtest GTest::gtest_main)
    target_compile_features(test_soak PRIVATE cxx_std_17)
    target_include_directories(test_soak PRIVATE ${CMAKE_SOURCE_DIR}/test)
    add_test(NAME test_soak COMMAND test_soak)
endif()

# API 文档（需要 doxygen）：cmake -B build -DBUILD_DOCS=ON && cmake --build build --target docs
# 产物在 build/docs/html/index.html
option(BUILD_DOCS "Build API docs (requires doxygen)" OFF)
if(BUILD_DOCS)
    find_package(Doxygen REQUIRED)
    # Doxygen 不清理输出目录：重命名符号后旧页面会留在那里，和新页面混在一起，
    # 所以先删掉整个输出目录（路径跟 Doxyfile 的 OUTPUT_DIRECTORY 保持一致）
    add_custom_target(
        docs
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_SOURCE_DIR}/build/docs
        COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Doxyfile
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "生成 API 文档 → build/docs/html/index.html"
        VERBATIM)
endif()

# 性能基准：Google Benchmark（可选依赖，未安装则跳过）。
# 需要：sudo apt install libbenchmark-dev
# 用法：./build-release/logger_benchmark [--benchmark_filter=...] [--benchmark_repetitions=N]
option(BUILD_BENCHMARKS "Build benchmarks" ON)
if(BUILD_BENCHMARKS)
    find_package(benchmark QUIET)
    if(benchmark_FOUND)
        add_executable(logger_benchmark ${CMAKE_SOURCE_DIR}/benchmark/logger_benchmark.cpp)
        target_link_libraries(logger_benchmark PRIVATE logger::logger benchmark::benchmark
                                                       benchmark::benchmark_main)
        target_compile_features(logger_benchmark PRIVATE cxx_std_17)
    else()
        message(STATUS "未找到 Google Benchmark，跳过性能基准（apt install libbenchmark-dev）")
    endif()
endif()
