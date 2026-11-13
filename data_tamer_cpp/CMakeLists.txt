cmake_minimum_required(VERSION 3.22)

# The ROS 2 package of 2.x is data_tamer_v2: it installs side by side with the released
# data_tamer_cpp 1.x. Plain CMake and conan installs are the package data_tamer.
project(data_tamer_v2 VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)

# As a subproject (add_subdirectory, FetchContent) data_tamer is a plain library: no
# ROS 2 package, no tests, examples or benchmarks, and no install rules unless asked.
option(DATA_TAMER_BUILD_TESTS "Build tests" ${PROJECT_IS_TOP_LEVEL})
option(DATA_TAMER_BUILD_EXAMPLES "Build examples" ${PROJECT_IS_TOP_LEVEL})
option(DATA_TAMER_BUILD_BENCHMARKS "Build benchmarks" ${PROJECT_IS_TOP_LEVEL})
option(DATA_TAMER_BUILD_ROS "Build the ROS 2 package when ament_cmake is found"
    ${PROJECT_IS_TOP_LEVEL})
option(DATA_TAMER_INSTALL "Generate the install rules" ${PROJECT_IS_TOP_LEVEL})
option(DATA_TAMER_USE_SYSTEM_MCAP
    "Use an installed MCAP package (find_package(mcap)) instead of downloading MCAP" OFF)
# OFF by default, so that a release builds with the next compiler (ROS buildfarm, conda,
# distributions); the presets and CI turn it ON.
option(DATA_TAMER_WERROR "Treat warnings in the library sources as errors" OFF)
option(BUILD_SHARED_LIBS "Build using shared libraries" OFF)

if(DATA_TAMER_BUILD_ROS)
    find_package(ament_cmake QUIET)
    if(NOT ament_cmake_FOUND)
        set(DATA_TAMER_BUILD_ROS OFF)
        message(STATUS "ament_cmake not found: building without ROS 2")
    endif()
endif()

# Under ROS 2, tests also follow BUILD_TESTING (defined by ament_cmake), as colcon and
# bloom expect: -DBUILD_TESTING=OFF builds no test.
if(DATA_TAMER_BUILD_ROS AND NOT BUILD_TESTING)
    set(DATA_TAMER_BUILD_TESTS OFF)
endif()

find_package(Threads REQUIRED)

# MCAP: under ROS 2 from mcap_vendor, whose libmcap contains the implementation.
# Otherwise the headers are downloaded with FetchContent (default) or come from an
# installed package (DATA_TAMER_USE_SYSTEM_MCAP, e.g. conan), and libdata_tamer compiles
# the implementation (src/sinks/mcap_sink.cpp), with zstd and lz4.
# DATA_TAMER_MCAP_TARGET gives tests and examples the MCAP headers.
if(DATA_TAMER_BUILD_ROS)
    find_package(mcap_vendor REQUIRED)
    set(DATA_TAMER_MCAP_TARGET mcap_vendor::mcap)
elseif(DATA_TAMER_USE_SYSTEM_MCAP)
    find_package(mcap REQUIRED)
    message(STATUS "MCAP from an installed package: ${mcap_DIR}")
    set(DATA_TAMER_MCAP_TARGET mcap::mcap)
else()
    list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
    # MODULE: only cmake/Find*.cmake define data_tamer::zstd and data_tamer::lz4 (they
    # try the upstream configs themselves), also with CMAKE_FIND_PACKAGE_PREFER_CONFIG.
    find_package(zstd MODULE REQUIRED)
    find_package(lz4 MODULE REQUIRED)
    # The MCAP C++ release archive, pinned by hash. Without network access (conda-forge,
    # distribution builds), pass -DFETCHCONTENT_SOURCE_DIR_DATA_TAMER_MCAP=<the unpacked
    # archive or a checkout of the tag>. It has no CMakeLists.txt: this only downloads it.
    include(FetchContent)
    if(POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif()
    FetchContent_Declare(data_tamer_mcap
        URL https://github.com/foxglove/mcap/archive/refs/tags/releases/cpp/v2.1.3.tar.gz
        URL_HASH SHA256=181a7c52cc982444cdef533087b321538b025fcc5eec258e0489d68b9ce53ded)
    FetchContent_MakeAvailable(data_tamer_mcap)
    set(DATA_TAMER_MCAP_INCLUDE_DIR "${data_tamer_mcap_SOURCE_DIR}/cpp/mcap/include")
    # Imported, so local to this directory and its subdirectories.
    add_library(data_tamer_mcap_headers INTERFACE IMPORTED)
    set_target_properties(data_tamer_mcap_headers PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${DATA_TAMER_MCAP_INCLUDE_DIR}")
    set(DATA_TAMER_MCAP_TARGET data_tamer_mcap_headers)
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
    src/sinks/fd_writable.cpp
    src/sinks/mcap_sink.cpp
    src/sinks/mcap_ring_sink.cpp
    $<$<BOOL:${DATA_TAMER_BUILD_ROS}>:src/sinks/ros2_publisher_sink.cpp>
)
add_library(data_tamer::data_tamer ALIAS data_tamer)

# Versioned shared library: libdata_tamer.so.<major>.<minor>.<patch>, with the
# SONAME libdata_tamer.so.<major> (the ABI generation). Any ABI-breaking change
# bumps the major version: see "ABI policy" in CLAUDE.md. Ignored by static builds.
set_target_properties(data_tamer PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})
# Under ROS 2 nothing may collide with the files of data_tamer_cpp 1.x in the same prefix
# (/opt/ros/<distro>), which has an unversioned libdata_tamer.so: libdata_tamer_v2.so.
# The headers (include/data_tamer_v2) differ from 1.x everywhere.
if(DATA_TAMER_BUILD_ROS)
    set_target_properties(data_tamer PROPERTIES OUTPUT_NAME ${PROJECT_NAME})
endif()

target_compile_options(data_tamer PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic -Wconversion -Wno-sign-conversion>
)
if(DATA_TAMER_WERROR)
    target_compile_options(data_tamer PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/WX,-Werror>)
endif()

# The library compiles as C++20; the installed headers stay valid C++17
# (consumers only need cxx_std_17), which tests/public_headers_cxx17.cpp
# enforces by compiling every public header as strict C++17.
target_compile_features(data_tamer PUBLIC cxx_std_17 PRIVATE cxx_std_20)
target_include_directories(data_tamer
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_compile_definitions(data_tamer PUBLIC DATA_TAMER_VERSION="${PROJECT_VERSION}")
target_link_libraries(data_tamer PUBLIC Threads::Threads)
if(DATA_TAMER_BUILD_ROS OR DATA_TAMER_USE_SYSTEM_MCAP)
    # PUBLIC: sinks/mcap_encoding.hpp includes the MCAP headers.
    target_link_libraries(data_tamer PUBLIC ${DATA_TAMER_MCAP_TARGET})
else()
    # sinks/mcap_encoding.hpp includes the MCAP headers: they are installed in a
    # directory of their own (include/data_tamer_v2_mcap/mcap), exported as an include
    # directory that holds nothing else, so that they cannot shadow another MCAP in the
    # same prefix nor expose anything else. SYSTEM: the library's warning flags do not
    # apply to MCAP (the include directories of an installed, imported target are SYSTEM
    # anyway).
    # zstd and lz4 are PRIVATE: a shared install exports no dependency, a static one
    # finds them again in data_tamerConfig.cmake.
    target_include_directories(data_tamer SYSTEM PUBLIC
        $<BUILD_INTERFACE:${DATA_TAMER_MCAP_INCLUDE_DIR}>)
    # BEFORE: ahead of <prefix>/include, where another MCAP may be installed.
    target_include_directories(data_tamer BEFORE PUBLIC
        $<INSTALL_INTERFACE:include/data_tamer_v2_mcap>)
    target_link_libraries(data_tamer PRIVATE data_tamer::zstd data_tamer::lz4)
endif()
# A static library linked into libdata_tamer.so (zstd from conan, for one) must not add
# its symbols to the exported ones, which the ABI policy freezes. Ignored when static.
target_link_options(data_tamer PRIVATE $<$<PLATFORM_ID:Linux>:LINKER:--exclude-libs,ALL>)

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

# Last: ament_package() must follow every other ament call.
if(DATA_TAMER_INSTALL)
    include(CMakePackageConfigHelpers)
    install(DIRECTORY include/ DESTINATION include)
    if(NOT DATA_TAMER_BUILD_ROS AND NOT DATA_TAMER_USE_SYSTEM_MCAP)
        install(DIRECTORY ${DATA_TAMER_MCAP_INCLUDE_DIR}/mcap
            DESTINATION include/data_tamer_v2_mcap)
        install(FILES ${data_tamer_mcap_SOURCE_DIR}/LICENSE
            DESTINATION include/data_tamer_v2_mcap/mcap)
    endif()

    install(
        TARGETS data_tamer
        EXPORT data_tamerTargets
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
        RUNTIME DESTINATION bin
    )

    if(DATA_TAMER_BUILD_ROS)
        ament_export_targets(data_tamerTargets HAS_LIBRARY_TARGET)
        ament_export_dependencies(Threads mcap_vendor rclcpp rclcpp_lifecycle data_tamer_msgs)

        # find_package(data_tamer 2) and data_tamer::data_tamer work under ROS 2 too: a
        # package "data_tamer" next to data_tamer_v2 forwards to data_tamer_v2::data_tamer.
        # data_tamer_cpp 1.x installs no package of that name under ROS 2.
        write_basic_package_version_file(
            ${CMAKE_CURRENT_BINARY_DIR}/ros/data_tamerConfigVersion.cmake
            VERSION ${PROJECT_VERSION}
            COMPATIBILITY SameMajorVersion)
        install(FILES cmake/data_tamerConfig-ros.cmake
            DESTINATION share/data_tamer/cmake
            RENAME data_tamerConfig.cmake)
        install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ros/data_tamerConfigVersion.cmake
            DESTINATION share/data_tamer/cmake)
        ament_package()
    else()
        set(INCLUDE_INSTALL_DIR include)
        get_target_property(DATA_TAMER_LIBRARY_TYPE data_tamer TYPE)
        if(DATA_TAMER_LIBRARY_TYPE STREQUAL "STATIC_LIBRARY")
            set(DATA_TAMER_STATIC ON)
        else()
            set(DATA_TAMER_STATIC OFF)
        endif()

        configure_package_config_file(
            ${CMAKE_CURRENT_SOURCE_DIR}/cmake/data_tamerConfig.cmake.in
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
        if(DATA_TAMER_STATIC AND NOT DATA_TAMER_USE_SYSTEM_MCAP)
            install(FILES cmake/Findzstd.cmake cmake/Findlz4.cmake
                DESTINATION lib/cmake/data_tamer)
        endif()
    endif()
endif()
