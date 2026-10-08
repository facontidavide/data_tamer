# One source list for both build flavours; the ROS-only test is appended below.
set(DATATAMER_TEST_SOURCES
    dt_tests.cpp
    custom_types_tests.cpp
    custom_type_name_tests.cpp
    types_registry_tests.cpp
    template_type_tests.cpp
    parser_tests.cpp
    parser_robustness_tests.cpp
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
    shared_state_tests.cpp
    channel_control_tests.cpp
    registration_atomicity_tests.cpp
    registration_fault_tests.cpp
    channel_capacity_tests.cpp
    payload_estimate_tests.cpp
    pool_exhausted_tests.cpp
    transaction_tests.cpp
    abi_tests.cpp
    locked_reference_tests.cpp
    serialize_me_tests.cpp
    serialize_me_bounds_tests.cpp
    serialize_me_size_tests.cpp
    serialize_me_api_tests.cpp
    string_encoding_tests.cpp
    mcap_encoding_tests.cpp
    mcap_ring_sink_tests.cpp
    mcap_sink_reopen_tests.cpp
    mcap_sink_rollover_failure_tests.cpp
    mcap_ring_sink_time_jump_tests.cpp
    mcap_ring_sink_callback_tests.cpp
    mcap_negative_timestamp_tests.cpp
    wire_format_tests.cpp
    name_tests.cpp
    lock_order_tests.cpp
    visibility_tests.cpp
    worker_lifecycle_tests.cpp
    announcement_tests.cpp
    rt_syscall_tests.cpp
    multi_producer_tests.cpp)

# look for gtest from ROS, but only when building for ROS 2
if(DATA_TAMER_BUILD_ROS)
    find_package(gtest_vendor QUIET)
    find_package(ament_cmake_gtest QUIET)
    if(NOT gtest_vendor_FOUND OR NOT ament_cmake_gtest_FOUND)
        message(STATUS "ROS gtest vendor not found, falling back to system GTest")
    endif()
endif()

if(DATA_TAMER_BUILD_ROS AND gtest_vendor_FOUND AND ament_cmake_gtest_FOUND)
    set(DATA_TAMER_USE_AMENT_GTEST ON)
    list(APPEND DATATAMER_TEST_SOURCES ros2_publisher_tests.cpp)
else()
    find_package(GTest REQUIRED)
    include(GoogleTest)

    # Optional launcher for the test binaries (the tsan preset uses it to run under
    # `setarch -R`). CROSSCOMPILING_EMULATOR is honoured both by test discovery
    # and by the discovered tests, so per-test granularity is kept.
    set(DATA_TAMER_TEST_WRAPPER "" CACHE FILEPATH
        "Optional launcher that execs the test binary (used for TSAN)")
endif()

# data_tamer_add_gtest(<target> SOURCES <files...> [LIBRARIES <targets...>])
# A gtest program with one CTest test per gtest test: ament_add_gtest under ROS 2, an
# executable with GTest::gtest_main and test discovery otherwise. The plain build
# also runs it under DATA_TAMER_TEST_WRAPPER and turns a deprecated call into an error.
function(data_tamer_add_gtest target)
    cmake_parse_arguments(ARG "" "" "SOURCES;LIBRARIES" ${ARGN})
    if(DATA_TAMER_USE_AMENT_GTEST)
        ament_add_gtest(${target} ${ARG_SOURCES})
        # ament_add_gtest links with the plain signature; CMake forbids mixing it with
        # keywords.
        if(ARG_LIBRARIES)
            target_link_libraries(${target} ${ARG_LIBRARIES})
        endif()
    else()
        add_executable(${target} ${ARG_SOURCES})
        target_compile_options(${target} PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang>:-Werror=deprecated-declarations>)
        target_link_libraries(${target} PRIVATE ${ARG_LIBRARIES} GTest::gtest_main)
        if(DATA_TAMER_TEST_WRAPPER)
            set_target_properties(${target} PROPERTIES
                CROSSCOMPILING_EMULATOR "${DATA_TAMER_TEST_WRAPPER}")
        endif()
        gtest_discover_tests(${target} DISCOVERY_MODE PRE_TEST)
    endif()
endfunction()

# data_tamer_strict_cxx17(<target> [WARNINGS])
# Compiles the target as strict C++17, without compiler extensions. WARNINGS also makes
# every warning an error, as for the library.
function(data_tamer_strict_cxx17 target)
    cmake_parse_arguments(ARG "WARNINGS" "" "" ${ARGN})
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    if(ARG_WARNINGS)
        target_compile_options(${target} PRIVATE
            $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
            $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic -Werror>)
    endif()
endfunction()

# data_tamer_add_build_test(<test> <target>)
# A test that builds the target, which is excluded from the default build, and passes
# when the build succeeds.
function(data_tamer_add_build_test name target)
    add_test(NAME ${name}
        COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --config $<CONFIG>
                --target ${target})
    # The nested builds share one build directory: run them one at a time.
    set_tests_properties(${name} PROPERTIES RESOURCE_LOCK build_tree)
endfunction()

data_tamer_add_gtest(datatamer_test
    SOURCES ${DATATAMER_TEST_SOURCES}
    LIBRARIES data_tamer ${DATA_TAMER_MCAP_TARGET})
# Tests are built as C++20 like the library.
target_compile_features(datatamer_test PRIVATE cxx_std_20)

# SerializeMe's byte-swapping path, built as for a big endian host. It is its own
# executable: SerializeMe's inline functions differ from the ones in datatamer_test.
data_tamer_add_gtest(serialize_me_big_endian_test
    SOURCES serialize_me_big_endian_tests.cpp)
target_compile_definitions(serialize_me_big_endian_test PRIVATE SERIALIZE_LITTLEENDIAN=0)
target_include_directories(serialize_me_big_endian_test
    PRIVATE ${PROJECT_SOURCE_DIR}/include)
data_tamer_strict_cxx17(serialize_me_big_endian_test)

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
data_tamer_strict_cxx17(public_headers_cxx17)

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
    data_tamer_strict_cxx17(${target} WARNINGS)
    target_compile_options(${target} PRIVATE
        $<$<BOOL:${DATA_TAMER_HAS_WMISMATCHED_TAGS}>:-Wmismatched-tags>)
endforeach()

# An object library of a source in tests/compile_fail/, built under the strict flags.
function(data_tamer_add_probe target source)
    add_library(${target} OBJECT ${ARGN} compile_fail/${source})
    target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_link_libraries(${target} PRIVATE ${DATA_TAMER_MCAP_TARGET})
    data_tamer_strict_cxx17(${target} WARNINGS)
endfunction()

# Compile-fail checks. A probe in tests/compile_fail/ compiles as written
# (<stem>_control, part of the default build). <name>_fail adds `definition`, which
# breaks the probe; the test builds it and passes when the output matches REGEX, the
# message the user gets, so a build that fails for another reason does not pass.
# OPTIONS are compile options of both variants, FAIL_OPTIONS of the failing one. All
# calls on one probe share its OPTIONS: the control target is made by the first call and
# takes that call's.
function(data_tamer_compile_fail_test name source definition)
    cmake_parse_arguments(ARG "" "REGEX" "OPTIONS;FAIL_OPTIONS" ${ARGN})
    if(NOT ARG_REGEX)
        message(FATAL_ERROR "${name}: a compile-fail test needs a REGEX")
    endif()
    get_filename_component(stem ${source} NAME_WE)
    if(NOT TARGET ${stem}_control)
        data_tamer_add_probe(${stem}_control ${source})
        target_compile_options(${stem}_control PRIVATE ${ARG_OPTIONS})
    endif()
    data_tamer_add_probe(${name}_fail ${source} EXCLUDE_FROM_ALL)
    target_compile_options(${name}_fail PRIVATE ${ARG_OPTIONS} ${ARG_FAIL_OPTIONS})
    if(definition)
        target_compile_definitions(${name}_fail PRIVATE ${definition})
    endif()
    data_tamer_add_build_test(${name} ${name}_fail)
    set_tests_properties(${name} PROPERTIES PASS_REGULAR_EXPRESSION "${ARG_REGEX}")
endfunction()

# What GCC, Clang and MSVC say about a discarded [[nodiscard]] result.
set(DATA_TAMER_NODISCARD_REGEX "ignoring return value|discarding return value")

# Each case of library_nodiscard.cpp and parser_nodiscard.cpp ignores one result.
foreach(case POOL_TRY_ACQUIRE POOL_ADOPT TO_STR TO_YAML RENDER_SCHEMA REQUEST_DUMP
             TRY_SET_ENABLED CREATE NUMBER_OF_SINKS)
    string(TOLOWER ${case} case_name)
    data_tamer_compile_fail_test(${case_name}_result_is_nodiscard
        library_nodiscard.cpp DT_IGNORE_${case}
        REGEX "${DATA_TAMER_NODISCARD_REGEX}")
endforeach()
foreach(case BUILD_SCHEMA_FROM_TEXT BUILD_SCHEMA_FROM_YAML TO_TEXT GET_BIT
             SPLIT_MCAP_MESSAGE PARSE_SNAPSHOT)
    string(TOLOWER ${case} case_name)
    data_tamer_compile_fail_test(${case_name}_result_is_nodiscard
        parser_nodiscard.cpp DT_IGNORE_${case}
        REGEX "${DATA_TAMER_NODISCARD_REGEX}")
endforeach()

data_tamer_compile_fail_test(atomic_word_table_const_is_read_only
    atomic_word_table_const.cpp DT_WRITE_THROUGH_CONST
    REGEX "discards qualifiers|no matching member function|cannot convert 'this' pointer")
data_tamer_compile_fail_test(fixed_size_of_an_undescribed_type_is_rejected
    get_fixed_size.cpp DT_UNDESCRIBED_TYPE
    REGEX "Missing TypeDefinition")
data_tamer_compile_fail_test(serializer_is_not_built_from_a_name_implicitly
    custom_serializer_constructor.cpp DT_IMPLICIT_NAME
    REGEX "conversion from|no viable conversion|cannot convert")
foreach(case VALUE VECTOR MEMBER LOGGED)
    string(TOLOWER ${case} case_name)
    data_tamer_compile_fail_test(long_double_${case_name}_is_rejected
        long_double_registration.cpp DT_LONG_DOUBLE_${case} REGEX "has no wire type")
endforeach()

data_tamer_compile_fail_test(dropped_snapshots_is_deprecated
    dropped_snapshots_deprecated.cpp DT_CALL_DEPRECATED
    REGEX "deprecated-declarations|declared deprecated")

data_tamer_compile_fail_test(endian_swap_of_long_double_is_rejected
    endian_swap_size.cpp DT_SWAP_LONG_DOUBLE OPTIONS -DSERIALIZE_LITTLEENDIAN=0
    REGEX "accepts only 1, 2, 4 and 8 byte types")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    data_tamer_compile_fail_test(parser_needs_a_little_endian_host
        parser_byte_order.cpp ""
        FAIL_OPTIONS -U__BYTE_ORDER__ -D__BYTE_ORDER__=__ORDER_BIG_ENDIAN__
        REGEX "needs a little endian host")
endif()

# logged_value.hpp must work on its own. The program includes nothing else, so it links
# only if the header defines the members it calls; a test builds it on demand.
add_executable(logged_value_alone EXCLUDE_FROM_ALL logged_value_alone.cpp)
target_link_libraries(logged_value_alone PRIVATE data_tamer)
data_tamer_strict_cxx17(logged_value_alone)
data_tamer_add_build_test(logged_value_header_alone logged_value_alone)

# The standalone parser is copied into other projects: with its templates instantiated
# it must compile without a warning under the strict flags of this library.
add_library(parser_header_warnings OBJECT parser_header_warnings.cpp)
target_include_directories(parser_header_warnings PRIVATE ${PROJECT_SOURCE_DIR}/include)
data_tamer_strict_cxx17(parser_header_warnings WARNINGS)
target_compile_options(parser_header_warnings PRIVATE
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wconversion -Wno-sign-conversion>)

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
