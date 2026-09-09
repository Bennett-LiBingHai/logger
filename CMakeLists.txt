cmake_minimum_required(VERSION 3.16)
project(
    logger
    VERSION 1.0.0
    LANGUAGES CXX C
)

# 消毒器开关：thread / address / undefined / leak（逗号分隔可组合，注意 thread 不能与 address/leak 混用）
# 注：仅 thread 需要 sudo sysctl vm.mmap_rnd_bits=28（address/undefined/leak 不需要）
set(LOGGER_SANITIZE "" CACHE STRING "Sanitizer(s) to enable: thread, address, undefined, leak")
if(LOGGER_SANITIZE)
    message(STATUS "Sanitizer enabled: ${LOGGER_SANITIZE}")
    add_compile_options(-fsanitize=${LOGGER_SANITIZE} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${LOGGER_SANITIZE})
endif()

set(SRC_FILES
    ${CMAKE_SOURCE_DIR}/src/formatter/text_formatter.cpp
    ${CMAKE_SOURCE_DIR}/src/formatter/json_formatter.cpp
    ${CMAKE_SOURCE_DIR}/src/sink/console_sink.cpp
    ${CMAKE_SOURCE_DIR}/src/sink/file_sink.cpp
    ${CMAKE_SOURCE_DIR}/src/field.cpp
    ${CMAKE_SOURCE_DIR}/src/logger.cpp
    ${CMAKE_SOURCE_DIR}/src/utils.cpp
    )

add_library(logger STATIC ${SRC_FILES})

target_compile_features(logger PUBLIC cxx_std_17)

target_include_directories(logger PUBLIC ${CMAKE_SOURCE_DIR}/include)

find_package(Threads REQUIRED)
target_link_libraries(logger PUBLIC Threads::Threads)

# 可运行示例
add_executable(logger_example ${CMAKE_SOURCE_DIR}/examples/basic_usage.cpp)
target_link_libraries(logger_example PRIVATE logger)
target_compile_features(logger_example PRIVATE cxx_std_17)


option(BUILD_TESTS "Build tests" ON)
if(BUILD_TESTS)
    enable_testing()
    find_package(GTest REQUIRED)

    set(TEST_SOURCES
        ${CMAKE_SOURCE_DIR}/test/unit/test_level.cpp
        ${CMAKE_SOURCE_DIR}/test/unit/test_config.cpp
        ${CMAKE_SOURCE_DIR}/test/unit/test_formatter.cpp
        ${CMAKE_SOURCE_DIR}/test/unit/test_field.cpp
        ${CMAKE_SOURCE_DIR}/test/unit/test_format.cpp
        ${CMAKE_SOURCE_DIR}/test/unit/test_json_formatter.cpp
        ${CMAKE_SOURCE_DIR}/test/unit/test_file_sink.cpp
        ${CMAKE_SOURCE_DIR}/test/unit/test_logger.cpp
        ${CMAKE_SOURCE_DIR}/test/integration/test_concurrency.cpp
    )

    add_executable(test_logger ${TEST_SOURCES})
    target_link_libraries(test_logger PRIVATE logger GTest::gtest GTest::gtest_main)
    target_compile_features(test_logger PRIVATE cxx_std_17)
    target_include_directories(test_logger PRIVATE ${CMAKE_SOURCE_DIR}/test)
    add_test(NAME test_logger COMMAND test_logger)

    # 手动观察 ConsoleSink 按级别路由（Error+ → stderr，其余 → stdout），独立二进制避免污染单例
    add_executable(test_console ${CMAKE_SOURCE_DIR}/test/integration/test_console_output.cpp)
    target_link_libraries(test_console PRIVATE logger GTest::gtest GTest::gtest_main)
    target_compile_features(test_console PRIVATE cxx_std_17)
    target_include_directories(test_console PRIVATE ${CMAKE_SOURCE_DIR}/test)
    add_test(NAME test_console COMMAND test_console)

    # 异步日志，独立二进制避免污染单例的同步/异步状态
    add_executable(test_async ${CMAKE_SOURCE_DIR}/test/integration/test_async.cpp)
    target_link_libraries(test_async PRIVATE logger GTest::gtest GTest::gtest_main)
    target_compile_features(test_async PRIVATE cxx_std_17)
    target_include_directories(test_async PRIVATE ${CMAKE_SOURCE_DIR}/test)
    add_test(NAME test_async COMMAND test_async)

    # 异步优雅关闭，独立二进制（close() 会停止异步线程，单例不可复用）
    add_executable(test_async_close ${CMAKE_SOURCE_DIR}/test/integration/test_async_close.cpp)
    target_link_libraries(test_async_close PRIVATE logger GTest::gtest GTest::gtest_main)
    target_compile_features(test_async_close PRIVATE cxx_std_17)
    target_include_directories(test_async_close PRIVATE ${CMAKE_SOURCE_DIR}/test)
    add_test(NAME test_async_close COMMAND test_async_close)
endif()

# 性能基准（零外部依赖，自研计时器；不在 CTest 中默认运行，手动执行 ./build/logger_benchmark）
option(BUILD_BENCHMARKS "Build benchmarks" ON)
if(BUILD_BENCHMARKS)
    add_executable(logger_benchmark ${CMAKE_SOURCE_DIR}/benchmark/logger_benchmark.cpp)
    target_link_libraries(logger_benchmark PRIVATE logger)
    target_compile_features(logger_benchmark PRIVATE cxx_std_17)
endif()
