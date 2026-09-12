# Every benchmark links the library and the test allocation hook, so that
# allocations on the measured thread can be counted.
function(CompileBenchmark TARGET)
    add_executable(${TARGET} ${TARGET}.cpp ${PROJECT_SOURCE_DIR}/tests/alloc_counter.cpp)
    target_include_directories(${TARGET}
        PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        PRIVATE ${PROJECT_SOURCE_DIR}/tests)
    target_link_libraries(${TARGET} data_tamer ${ARGN})
endfunction()

find_package(Threads REQUIRED)

CompileBenchmark(data_tamer_benchmark benchmark::benchmark)
CompileBenchmark(sink_latency Threads::Threads)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    CompileBenchmark(rt_latency Threads::Threads)
endif()

if(DATA_TAMER_BUILD_TESTS AND TARGET rt_latency)
    add_test(
        NAME rt_latency.invalid_cli
        COMMAND ${CMAKE_COMMAND}
            -DRT_LATENCY=$<TARGET_FILE:rt_latency>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/rt_latency_cli_test.cmake)
endif()
