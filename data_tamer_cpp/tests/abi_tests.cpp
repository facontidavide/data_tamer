#include "data_tamer/data_sink.hpp"
#include "data_tamer/sinks/mcap_sink.hpp"

#ifdef USING_ROS2
#include "data_tamer/sinks/ros2_publisher_sink.hpp"
#endif

#include <gtest/gtest.h>

#include <memory>

using namespace DataTamer;

TEST(ABI, SinksAreOnlyOnePointerLargerThanTheBase)
{
  static_assert(sizeof(MCAPSink) == sizeof(DataSinkBase) + sizeof(std::unique_ptr<int>),
                "MCAPSink grew a member outside its Pimpl");
#ifdef USING_ROS2
  static_assert(sizeof(ROS2PublisherSink) ==
                    sizeof(DataSinkBase) + sizeof(std::unique_ptr<int>),
                "ROS2PublisherSink grew a member outside its Pimpl");
#endif
}
