# usage: cmake -DNM=<nm> -DLIB=<shared object> -P check_no_stats_symbol.cmake
#
# The inline stats() functions must not be exported by a binary that uses them:
# an exported weak copy would serve every other binary loaded in the process,
# built against another version of the header.
execute_process(COMMAND "${NM}" -D -C --defined-only "${LIB}"
    OUTPUT_VARIABLE symbols RESULT_VARIABLE status)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "nm failed on ${LIB}")
endif()
if(NOT symbols MATCHES "data_tamer_stats_probe")
    message(FATAL_ERROR "the probe symbol is missing from ${LIB}:\n${symbols}")
endif()
if(symbols MATCHES "[^\n]*::stats\\(\\)[^\n]*")
    message(FATAL_ERROR "stats() is exported by ${LIB}:\n${CMAKE_MATCH_0}")
endif()
