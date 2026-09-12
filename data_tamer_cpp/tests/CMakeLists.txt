# One source list for both build flavours; the ROS-only test is appended below.
set(DATATAMER_TEST_SOURCES
    dt_tests.cpp
    custom_types_tests.cpp
    parser_tests.cpp
    add_remove_sink_tests.cpp
    trait_tests.cpp
    alloc_counter.cpp
    alloc_counter_tests.cpp
    write_mutex_tests.cpp
    snapshot_pool_tests.cpp
    sink_queue_tests.cpp
    value_ptr_tests.cpp
    logged_value_tests.cpp
    shared_state_tests.cpp
    channel_control_tests.cpp
    channel_capacity_tests.cpp
    transaction_tests.cpp
    abi_tests.cpp
    locked_reference_tests.cpp
    serialize_me_tests.cpp
    wire_format_tests.cpp)

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

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
    add_test(NAME python_reference_decoder
        COMMAND Python3::Interpreter -m unittest discover -s ${PROJECT_SOURCE_DIR}/../python -p "test_*.py")
endif()

# Every installed header must compile as strict C++17: the library itself is
# built as C++20, and this object library is what stops a C++20-only construct
# from leaking into the public interface.
# The check covers the core headers only. The ROS 2 sink header is excluded on
# purpose: it includes rclcpp, whose exported compile features already require
# C++20 on recent distributions, so it cannot promise C++17 regardless of us.
add_library(public_headers_cxx17 OBJECT public_headers_cxx17.cpp)
target_include_directories(public_headers_cxx17 PRIVATE ${PROJECT_SOURCE_DIR}/include)
set_target_properties(public_headers_cxx17 PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
