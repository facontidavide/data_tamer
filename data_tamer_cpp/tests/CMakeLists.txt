# look for gtest from ROS, but do not fail if it is not found
find_package(gtest_vendor QUIET)
find_package(ament_cmake_gtest QUIET)

if(gtest_vendor_FOUND AND ament_cmake_gtest_FOUND)
    ament_add_gtest(datatamer_test
        dt_tests.cpp
        custom_types_tests.cpp
        parser_tests.cpp
        ros2_publisher_tests.cpp
        serialize_me_tests.cpp
        trait_tests.cpp
        wire_format_tests.cpp
        eigen_tests.cpp)

    target_include_directories(datatamer_test
        PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)

    target_link_libraries(datatamer_test data_tamer)
else()
    if (NOT gtest_vendor_FOUND)
        message(STATUS "gtest vendor not found, falling back to system GTest")
    endif()
    if (NOT ament_cmake_gtest_FOUND)
        message(STATUS "ament_cmake_gtest not found, falling back to system GTest")
    endif()
    # we must have gtest either from the ROS vendor (above) or on the system
    find_package(GTest REQUIRED)

    include(GoogleTest)

    add_executable(datatamer_test
        dt_tests.cpp
        custom_types_tests.cpp
        parser_tests.cpp
        serialize_me_tests.cpp
        add_remove_sink_tests.cpp
        wire_format_tests.cpp
        eigen_tests.cpp)
    gtest_discover_tests(datatamer_test DISCOVERY_MODE PRE_TEST)

    target_include_directories(datatamer_test
        PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)

    target_link_libraries(datatamer_test data_tamer GTest::gtest_main)

    add_test(NAME datatamer_test COMMAND $<TARGET_FILE:datatamer_test>)
endif()

# The wire-format test reads back the MCAP file written by MCAPSink.
if(USE_VENDORED_MCAP)
    target_include_directories(datatamer_test PRIVATE ${PROJECT_SOURCE_DIR}/3rdparty/mcap/include)
elseif(NOT DATA_TAMER_BUILD_ROS)
    target_link_libraries(datatamer_test mcap::mcap)
endif()

# Golden vectors for docs/wire_format.md, shared with the Python reference decoder.
target_compile_definitions(datatamer_test PRIVATE
    DATA_TAMER_WIRE_FORMAT_DIR="${PROJECT_SOURCE_DIR}/../docs/wire_format")

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
    add_test(NAME python_reference_decoder
        COMMAND Python3::Interpreter -m unittest discover -s ${PROJECT_SOURCE_DIR}/../python -p "test_*.py")
endif()
