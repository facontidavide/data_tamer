# Examples are consumers: build them as plain C++17 to prove the public API needs
# no more. Under ROS 2, rclcpp may itself demand C++20 and CMake raises it.
function(CompileExample TARGET)
    add_executable(${TARGET} ${TARGET}.cpp)
    target_link_libraries(${TARGET} PRIVATE data_tamer)
    set_target_properties(${TARGET} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endfunction()

CompileExample(T01_basic_example)
CompileExample(T02_custom_types)
CompileExample(T03_mcap_writer)
CompileExample(mcap_1m_per_sec)

CompileExample(mcap_reader)
target_link_libraries(mcap_reader PRIVATE ${DATA_TAMER_MCAP_TARGET})

if(DATA_TAMER_BUILD_ROS)
    CompileExample(ros2_publisher)
    install(TARGETS ros2_publisher DESTINATION lib/${PROJECT_NAME})
endif()
