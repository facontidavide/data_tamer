# One source list for both build flavours; the ROS-only test is appended below.
set(DATATAMER_TEST_SOURCES
    dt_tests.cpp
    custom_types_tests.cpp
    parser_tests.cpp
    add_remove_sink_tests.cpp
    trait_tests.cpp
    type_definition_trait_tests.cpp
    alloc_counter.cpp
    alloc_counter_tests.cpp
    write_mutex_tests.cpp
    snapshot_pool_tests.cpp
    sink_queue_tests.cpp
    sink_lifecycle_tests.cpp
    channel_stats_tests.cpp
    value_ptr_tests.cpp
    logged_value_tests.cpp
    shared_state_tests.cpp
    channel_control_tests.cpp
    channel_capacity_tests.cpp
    transaction_tests.cpp
    abi_tests.cpp
    locked_reference_tests.cpp
    serialize_me_tests.cpp
    mcap_encoding_tests.cpp
    mcap_ring_sink_tests.cpp
    wire_format_tests.cpp
    name_tests.cpp)

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

# Golden vectors for docs/wire_format.md, shared with the Python reference decoder.
target_compile_definitions(datatamer_test PRIVATE
    DATA_TAMER_WIRE_FORMAT_DIR="${PROJECT_SOURCE_DIR}/../docs/wire_format")

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
