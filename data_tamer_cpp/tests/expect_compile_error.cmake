# Builds TARGET in BUILD_DIR. Passes only if the build fails and the compiler output
# contains EXPECTED, so a probe that breaks for another reason is not a pass.
execute_process(
    COMMAND ${CMAKE_COMMAND} --build ${BUILD_DIR} --target ${TARGET}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)
if(result EQUAL 0)
    message(FATAL_ERROR "${TARGET} compiled, but it must be rejected")
endif()
if(NOT "${out}${err}" MATCHES "${EXPECTED}")
    message(FATAL_ERROR "${TARGET} failed, but not with '${EXPECTED}':\n${out}${err}")
endif()
