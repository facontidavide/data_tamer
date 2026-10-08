# One source list for both build flavours; the ROS-only test is appended below.
set(DATATAMER_TEST_SOURCES
    dt_tests.cpp
    custom_types_tests.cpp
    custom_type_name_tests.cpp
    types_registry_tests.cpp
    template_type_tests.cpp
    parser_tests.cpp
    add_remove_sink_tests.cpp
    trait_tests.cpp
    type_definition_trait_tests.cpp
    alloc_counter.cpp
    alloc_counter_tests.cpp
    write_mutex_tests.cpp
    write_mutex_spin_tests.cpp
    snapshot_pool_tests.cpp
    sink_queue_tests.cpp
    sink_lifecycle_tests.cpp
    channel_stats_tests.cpp
    value_ptr_tests.cpp
    value_ptr_bounds_tests.cpp
    basic_type_tests.cpp
    logged_value_tests.cpp
    logged_value_api_tests.cpp
    shared_state_tests.cpp
    channel_control_tests.cpp
    registration_atomicity_tests.cpp
    channel_capacity_tests.cpp
    payload_estimate_tests.cpp
    pool_exhausted_tests.cpp
    transaction_tests.cpp
    abi_tests.cpp
    locked_reference_tests.cpp
    serialize_me_tests.cpp
    mcap_encoding_tests.cpp
    mcap_ring_sink_tests.cpp
    wire_format_tests.cpp
    name_tests.cpp
    registry_limit_tests.cpp
    lock_order_tests.cpp
    visibility_tests.cpp
    worker_lifecycle_tests.cpp
    announcement_tests.cpp
    rt_syscall_tests.cpp
    multi_producer_tests.cpp
    sink_worker_api_tests.cpp)

# look for gtest from ROS, but only when building for ROS 2
if(DATA_TAMER_BUILD_ROS)
    find_package(gtest_vendor QUIET)
    find_package(ament_cmake_gtest QUIET)
    if(NOT gtest_vendor_FOUND OR NOT ament_cmake_gtest_FOUND)
        message(STATUS "ROS gtest vendor not found, falling back to system GTest")
    endif()
endif()

if(DATA_TAMER_BUILD_ROS AND gtest_vendor_FOUND AND ament_cmake_gtest_FOUND)
    ament_add_gtest(datatamer_test ${DATATAMER_TEST_SOURCES} ros2_publisher_tests.cpp)
    # ament_add_gtest links with the plain signature; CMake forbids mixing it with keywords.
    target_link_libraries(datatamer_test data_tamer ${DATA_TAMER_MCAP_TARGET})
else()
    find_package(GTest REQUIRED)
    include(GoogleTest)

    add_executable(datatamer_test ${DATATAMER_TEST_SOURCES})
    target_compile_options(datatamer_test PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-Werror=deprecated-declarations>)
    target_link_libraries(datatamer_test PRIVATE data_tamer ${DATA_TAMER_MCAP_TARGET} GTest::gtest_main)

    # Optional launcher for the test binary (the tsan preset uses it to run under
    # `setarch -R`). CROSSCOMPILING_EMULATOR is honoured both by test discovery
    # and by the discovered tests, so per-test granularity is kept.
    set(DATA_TAMER_TEST_WRAPPER "" CACHE FILEPATH "Optional launcher that execs the test binary (used for TSAN)")
    if(DATA_TAMER_TEST_WRAPPER)
        set_target_properties(datatamer_test PROPERTIES CROSSCOMPILING_EMULATOR "${DATA_TAMER_TEST_WRAPPER}")
    endif()
    gtest_discover_tests(datatamer_test DISCOVERY_MODE PRE_TEST)
endif()

# Tests are built as C++20 like the library.
target_compile_features(datatamer_test PRIVATE cxx_std_20)

# The allocation-failure tests replace operator new, so they are a program of their own.
if(DATA_TAMER_BUILD_ROS AND gtest_vendor_FOUND AND ament_cmake_gtest_FOUND)
    ament_add_gtest(datatamer_fault_test registration_fault_tests.cpp)
    target_link_libraries(datatamer_fault_test data_tamer ${DATA_TAMER_MCAP_TARGET})
else()
    add_executable(datatamer_fault_test registration_fault_tests.cpp)
    target_link_libraries(datatamer_fault_test
        PRIVATE data_tamer ${DATA_TAMER_MCAP_TARGET} GTest::gtest_main)
    if(DATA_TAMER_TEST_WRAPPER)
        set_target_properties(datatamer_fault_test PROPERTIES
            CROSSCOMPILING_EMULATOR "${DATA_TAMER_TEST_WRAPPER}")
    endif()
    gtest_discover_tests(datatamer_fault_test DISCOVERY_MODE PRE_TEST)
endif()
target_compile_features(datatamer_fault_test PRIVATE cxx_std_20)

# Golden vectors for docs/wire_format.md, shared with the Python reference decoder.
target_compile_definitions(datatamer_test PRIVATE
    DATA_TAMER_WIRE_FORMAT_DIR="${PROJECT_SOURCE_DIR}/../docs/wire_format")

# A consumer compiled with -fvisibility=hidden, loaded by visibility_tests.cpp. It can
# only share the library's state when libdata_tamer is a shared library (always under
# ROS 2, BUILD_SHARED_LIBS=ON otherwise); without it the test skips.
get_target_property(DATA_TAMER_LIBRARY_TYPE data_tamer TYPE)
if(UNIX AND DATA_TAMER_LIBRARY_TYPE STREQUAL "SHARED_LIBRARY")
    add_library(hidden_visibility_consumer MODULE hidden_visibility_consumer.cpp)
    set_target_properties(hidden_visibility_consumer PROPERTIES
        CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
    target_compile_features(hidden_visibility_consumer PRIVATE cxx_std_20)
    target_link_libraries(hidden_visibility_consumer PRIVATE data_tamer)
    add_dependencies(datatamer_test hidden_visibility_consumer)
    target_compile_definitions(datatamer_test PRIVATE
        DATA_TAMER_HIDDEN_CONSUMER="$<TARGET_FILE:hidden_visibility_consumer>")
endif()

# Resolve the package directory first: when it is a symlink (workspaces that
# link each package into src/), "<link>/../python" names a directory next to
# the link, not next to the real package, and Python normalizes ".." textually.
get_filename_component(DATA_TAMER_CPP_REAL_DIR "${PROJECT_SOURCE_DIR}" REALPATH)
get_filename_component(DATA_TAMER_REPO_DIR "${DATA_TAMER_CPP_REAL_DIR}" DIRECTORY)
set(DATA_TAMER_PYTHON_DIR "${DATA_TAMER_REPO_DIR}/python")
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND AND EXISTS "${DATA_TAMER_PYTHON_DIR}")
    add_test(NAME python_reference_decoder
        COMMAND Python3::Interpreter -m unittest discover -s ${DATA_TAMER_PYTHON_DIR} -p "test_*.py")
endif()

# The version number is kept in several files (tools/check_versions.py lists them).
if(Python3_FOUND AND EXISTS "${DATA_TAMER_REPO_DIR}/tools/check_versions.py")
    add_test(NAME version_consistency
        COMMAND Python3::Interpreter ${DATA_TAMER_REPO_DIR}/tools/check_versions.py)
endif()

# Every installed header must compile as strict C++17: the library itself is
# built as C++20, and this object library is what stops a C++20-only construct
# from leaking into the public interface.
# The check covers the core headers only. The ROS 2 sink header is excluded on
# purpose: it includes rclcpp, whose exported compile features already require
# C++20 on recent distributions, so it cannot promise C++17 regardless of us.
add_library(public_headers_cxx17 OBJECT public_headers_cxx17.cpp)
target_include_directories(public_headers_cxx17 PRIVATE ${PROJECT_SOURCE_DIR}/include)
# sinks/mcap_encoding.hpp includes the MCAP headers.
target_link_libraries(public_headers_cxx17 PRIVATE ${DATA_TAMER_MCAP_TARGET})
set_target_properties(public_headers_cxx17 PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

# include/data_tamer/fwd.hpp must declare the types exactly as the full headers
# define them, and work alone and in either include order. Object libraries only:
# nothing is linked. -Wmismatched-tags (a wrong class/struct key) is added where
# the compiler has it.
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag(-Wmismatched-tags DATA_TAMER_HAS_WMISMATCHED_TAGS)
foreach(variant FWD_ONLY FWD_FIRST FWD_LAST)
    string(TOLOWER ${variant} variant_name)
    set(target fwd_header_${variant_name})
    add_library(${target} OBJECT fwd_header_tests.cpp)
    target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_compile_definitions(${target} PRIVATE ${variant})
    target_link_libraries(${target} PRIVATE ${DATA_TAMER_MCAP_TARGET})
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_compile_options(${target} PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic -Werror>
        $<$<BOOL:${DATA_TAMER_HAS_WMISMATCHED_TAGS}>:-Wmismatched-tags>)
endforeach()

# Compile-fail checks. A probe in tests/compile_fail/ compiles as written
# (<stem>_control, part of the default build). The test defines a macro that breaks
# the probe: <name>_fail is left out of the default build, and the test passes when
# building it fails. Both variants share the flags, so the macro is the only cause.
# With a fourth argument the test passes when the build output matches that regular
# expression instead, which also checks the message the user gets.
function(data_tamer_add_probe target source)
    add_library(${target} OBJECT ${ARGN} compile_fail/${source})
    target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_link_libraries(${target} PRIVATE ${DATA_TAMER_MCAP_TARGET})
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_compile_options(${target} PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic -Werror>)
endfunction()

function(data_tamer_compile_fail_test name source definition)
    get_filename_component(stem ${source} NAME_WE)
    if(NOT TARGET ${stem}_control)
        data_tamer_add_probe(${stem}_control ${source})
    endif()
    data_tamer_add_probe(${name}_fail ${source} EXCLUDE_FROM_ALL)
    target_compile_definitions(${name}_fail PRIVATE ${definition})
    add_test(NAME ${name}
        COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target ${name}_fail)
    # The nested builds share one build directory: run them one at a time.
    set_tests_properties(${name} PROPERTIES RESOURCE_LOCK data_tamer_compile_fail)
    if(ARGC GREATER 3)
        set_tests_properties(${name} PROPERTIES PASS_REGULAR_EXPRESSION "${ARGV3}")
    else()
        set_tests_properties(${name} PROPERTIES WILL_FAIL TRUE)
    endif()
endfunction()

data_tamer_compile_fail_test(pool_try_acquire_result_is_nodiscard
    snapshot_pool_nodiscard.cpp DT_IGNORE_TRY_ACQUIRE)
data_tamer_compile_fail_test(pool_adopt_result_is_nodiscard
    snapshot_pool_nodiscard.cpp DT_IGNORE_ADOPT)

data_tamer_compile_fail_test(atomic_word_table_const_is_read_only
    atomic_word_table_const.cpp DT_WRITE_THROUGH_CONST)
data_tamer_compile_fail_test(fixed_size_of_an_undescribed_type_is_rejected
    get_fixed_size.cpp DT_UNDESCRIBED_TYPE)
data_tamer_compile_fail_test(serializer_is_not_built_from_a_name_implicitly
    custom_serializer_constructor.cpp DT_IMPLICIT_NAME)
foreach(function TO_STR TO_YAML RENDER_SCHEMA)
    string(TOLOWER ${function} function_name)
    data_tamer_compile_fail_test(${function_name}_result_is_nodiscard
        schema_nodiscard.cpp DT_IGNORE_${function})
endforeach()

foreach(case VALUE VECTOR MEMBER LOGGED)
    string(TOLOWER ${case} case_name)
    data_tamer_compile_fail_test(long_double_${case_name}_is_rejected
        long_double_registration.cpp DT_LONG_DOUBLE_${case} "has no wire type")
endforeach()

# Compile-fail tests: a probe that ignores a [[nodiscard]] result or calls a deprecated
# function, built with that diagnostic as an error. ctest builds the probe target and
# passes only if the build fails with the expected diagnostic.
function(data_tamer_add_compile_fail_test name source diagnostic_flag expected_text)
    add_library(${name} OBJECT EXCLUDE_FROM_ALL ${source})
    target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_compile_options(${name} PRIVATE ${diagnostic_flag})
    target_compile_definitions(${name} PRIVATE ${ARGN})
    set_target_properties(${name} PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    add_test(NAME compile_fail_${name}
        COMMAND ${CMAKE_COMMAND} -DBUILD_DIR=${CMAKE_BINARY_DIR} -DTARGET=${name}
                -DEXPECTED=${expected_text}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/expect_compile_error.cmake)
    # The tests build in the same tree: run them one at a time.
    set_tests_properties(compile_fail_${name} PROPERTIES RESOURCE_LOCK build_tree)
endfunction()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(ignored_result_source ${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/ignored_result.cpp)
    data_tamer_add_compile_fail_test(nodiscard_try_set_enabled ${ignored_result_source}
        -Werror=unused-result nodiscard PROBE_TRY_SET_ENABLED)
    data_tamer_add_compile_fail_test(nodiscard_create ${ignored_result_source}
        -Werror=unused-result nodiscard PROBE_CREATE)
    data_tamer_add_compile_fail_test(nodiscard_number_of_sinks ${ignored_result_source}
        -Werror=unused-result nodiscard PROBE_NUMBER_OF_SINKS)
    data_tamer_add_compile_fail_test(deprecated_dropped_snapshots
        ${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/deprecated_call.cpp
        -Werror=deprecated-declarations deprecated)
endif()

# logged_value.hpp must work on its own. The program includes nothing else, so it links
# only if the header defines the members it calls; a test builds it on demand.
add_executable(logged_value_alone EXCLUDE_FROM_ALL logged_value_alone.cpp)
target_link_libraries(logged_value_alone PRIVATE data_tamer)
set_target_properties(logged_value_alone PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
add_test(NAME logged_value_header_alone
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target logged_value_alone)
set_tests_properties(logged_value_header_alone PROPERTIES RESOURCE_LOCK build_tree)

# The inline stats() functions (LogChannel, SinkWorker, MCAPRingSink) are hidden:
# a shared object that calls them must not export a copy. The probe is built
# without optimization so that the calls are not inlined away, and is not
# linked against the library (the getters stay undefined, which a shared object
# allows): only its own symbol table matters.
find_program(DATA_TAMER_NM nm)
if(UNIX AND NOT APPLE AND DATA_TAMER_NM)
    add_library(stats_visibility_probe SHARED stats_visibility_probe.cpp)
    target_include_directories(stats_visibility_probe PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_compile_features(stats_visibility_probe PRIVATE cxx_std_20)
    target_compile_options(stats_visibility_probe PRIVATE -O0)
    target_link_libraries(stats_visibility_probe PRIVATE ${DATA_TAMER_MCAP_TARGET})
    add_test(NAME stats_functions_are_not_exported
        COMMAND ${CMAKE_COMMAND} -DNM=${DATA_TAMER_NM}
                -DLIB=$<TARGET_FILE:stats_visibility_probe>
                -P ${CMAKE_CURRENT_SOURCE_DIR}/check_no_stats_symbol.cmake)
endif()
