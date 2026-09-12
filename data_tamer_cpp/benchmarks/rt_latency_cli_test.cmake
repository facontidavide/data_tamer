function(expect_rejected)
    execute_process(
        COMMAND "${RT_LATENCY}" ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_QUIET
        ERROR_VARIABLE error
        TIMEOUT 2)
    if(NOT result STREQUAL "1" OR NOT error MATCHES "invalid option")
        message(FATAL_ERROR "${ARGN}: result=${result}, stderr=${error}")
    endif()
endfunction()

expect_rejected(--values)
expect_rejected(--seconds 0)
expect_rejected(--writers -1)
expect_rejected(--rate nope)
expect_rejected(--sinks 999999999999999999999)

expect_rejected(--sinks 9)

execute_process(
    COMMAND "${RT_LATENCY}" --values 2 --sinks 0 --seconds 1 --rate 1
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
    TIMEOUT 5)
if(NOT result STREQUAL "0")
    message(FATAL_ERROR "minimal run failed: ${result}, stderr=${error}")
endif()
foreach(counter write_lock_contended write_lock_wait_max_ns pool_exhausted payload_reallocations dropped_oversize attachment_drops)
    if(NOT output MATCHES "${counter}=[0-9]+")
        message(FATAL_ERROR "missing ${counter}: ${output}")
    endif()
endforeach()
if(NOT output MATCHES "allocations after warm-up: [0-9]+")
    message(FATAL_ERROR "missing exact allocation total: ${output}")
endif()
