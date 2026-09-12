cmake_minimum_required(VERSION 3.22)

project(data_tamer_cpp VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)

option(DATA_TAMER_BUILD_TESTS "Build tests" ${PROJECT_IS_TOP_LEVEL})
option(DATA_TAMER_BUILD_EXAMPLES "Build examples" ${PROJECT_IS_TOP_LEVEL})
option(DATA_TAMER_BUILD_BENCHMARKS "Build benchmarks" ${PROJECT_IS_TOP_LEVEL})
option(DATA_TAMER_BUILD_ROS "Build for ROS 2" ON)
option(BUILD_SHARED_LIBS "Build using shared libraries" OFF)

if(DATA_TAMER_BUILD_ROS)
    find_package(ament_cmake QUIET)
    if(NOT ament_cmake_FOUND)
        set(DATA_TAMER_BUILD_ROS OFF)
        message(WARNING "ament_cmake not found, building without ROS 2 support. Set DATA_TAMER_BUILD_ROS to OFF to disable this warning.")
    endif()
endif()

find_package(Threads REQUIRED)

# MCAP comes from mcap_vendor under ROS 2, from an installed package (conan,
# vcpkg, ...) when one is found, otherwise from the vendored copy.
# DATA_TAMER_MCAP_TARGET is what tests and examples link to read files directly.
if(DATA_TAMER_BUILD_ROS)
    find_package(mcap_vendor REQUIRED)
    set(DATA_TAMER_MCAP_TARGET mcap_vendor::mcap)
else()
    find_package(mcap QUIET)
    if(mcap_FOUND)
        message(STATUS "MCAP provided by an installed package")
        set(DATA_TAMER_MCAP_TARGET mcap::mcap)
    else()
        message(STATUS "MCAP from 3rdparty")
        add_subdirectory(3rdparty/mcap)
        set(DATA_TAMER_MCAP_TARGET mcap_lib)
    endif()
endif()

if(DATA_TAMER_BUILD_ROS AND NOT BUILD_SHARED_LIBS)
    message(STATUS "BUILD_SHARED_LIBS forced to ON when building with ROS 2")
    set(BUILD_SHARED_LIBS ON)
endif()

add_library(data_tamer
    src/channel.cpp
    src/data_tamer.cpp
    src/data_sink.cpp
    src/types.cpp
    src/sinks/mcap_sink.cpp
    $<$<BOOL:${DATA_TAMER_BUILD_ROS}>:src/sinks/ros2_publisher_sink.cpp>
)
add_library(data_tamer::data_tamer ALIAS data_tamer)

target_compile_options(data_tamer PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic -Wconversion -Wno-sign-conversion -Werror>
)

# The library compiles as C++20; the installed headers stay valid C++17
# (consumers only need cxx_std_17), which tests/public_headers_cxx17.cpp
# enforces by compiling every public header as strict C++17.
target_compile_features(data_tamer PUBLIC cxx_std_17 PRIVATE cxx_std_20)
target_include_directories(data_tamer
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/3rdparty>
)
target_compile_definitions(data_tamer PUBLIC DATA_TAMER_VERSION="${PROJECT_VERSION}")
target_link_libraries(data_tamer
    PUBLIC Threads::Threads
    PRIVATE ${DATA_TAMER_MCAP_TARGET}
)

set(INSTALL_TARGETS data_tamer)

if(DATA_TAMER_BUILD_ROS)
    message(STATUS "Compiling for ROS 2")
    find_package(rclcpp REQUIRED)
    find_package(rclcpp_lifecycle REQUIRED)
    find_package(data_tamer_msgs REQUIRED)

    target_compile_definitions(data_tamer PUBLIC USING_ROS2=1)
    # PUBLIC: the ROS 2 sink header includes rclcpp and the message types.
    target_link_libraries(data_tamer PUBLIC
        rclcpp::rclcpp
        rclcpp_lifecycle::rclcpp_lifecycle
        ${data_tamer_msgs_TARGETS}
    )

    ament_export_targets(data_tamerTargets HAS_LIBRARY_TARGET)
    ament_export_dependencies(mcap_vendor rclcpp rclcpp_lifecycle data_tamer_msgs)
    ament_package()
elseif(TARGET mcap_lib)
    list(APPEND INSTALL_TARGETS mcap_lib)
endif()

install(DIRECTORY include/ DESTINATION include)

install(
    TARGETS ${INSTALL_TARGETS}
    EXPORT data_tamerTargets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
    INCLUDES DESTINATION include
)

if(NOT DATA_TAMER_BUILD_ROS)
    include(CMakePackageConfigHelpers)

    set(INCLUDE_INSTALL_DIR include)
    # Consumers of a non-vendored MCAP must find it themselves (static link).
    if(mcap_FOUND)
        set(DATA_TAMER_FIND_MCAP "find_dependency(mcap)")
    endif()

    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/data_tamerConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/data_tamerConfig.cmake
        INSTALL_DESTINATION lib/cmake/data_tamer
        PATH_VARS INCLUDE_INSTALL_DIR)

    write_basic_package_version_file(
        data_tamerConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)

    install(EXPORT data_tamerTargets
        FILE data_tamerTargets.cmake
        NAMESPACE data_tamer::
        DESTINATION lib/cmake/data_tamer)

    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/data_tamerConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/data_tamerConfigVersion.cmake
        DESTINATION lib/cmake/data_tamer)
endif()

if(DATA_TAMER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(DATA_TAMER_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(DATA_TAMER_BUILD_BENCHMARKS)
    find_package(benchmark QUIET)
    if(benchmark_FOUND)
        add_subdirectory(benchmarks)
    else()
        message(STATUS "Google Benchmark library not found, benchmarks skipped")
    endif()
endif()
