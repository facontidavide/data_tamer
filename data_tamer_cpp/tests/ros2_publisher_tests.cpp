#include "data_tamer/data_tamer.hpp"
#include "data_tamer/sinks/ros2_publisher_sink.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace DataTamer;

TEST(DataTamerROS2Publisher, SharedPointer)
{
  auto node = std::make_shared<rclcpp::Node>("test_datatamer_shared_pointer");
  auto ros2_sink = ROS2PublisherSink::create(node, "test_shared_pointer");

  auto channel = ChannelsRegistry::Global().getChannel("channel_shared_pointer");

  channel->addDataSink(ros2_sink);

  double const value = 1.;
  channel->registerValue("value", &value);

  EXPECT_TRUE(channel->takeSnapshot());
}

TEST(DataTamerROS2Publisher, SharedPointerLifeCycle)
{
  auto lifecycle_node = std::make_shared<rclcpp_lifecycle::LifecycleNode>("test_"
                                                                          "datatamer_"
                                                                          "shared_"
                                                                          "pointer_"
                                                                          "lifecycle");
  auto ros2_sink = ROS2PublisherSink::create(lifecycle_node, "test_shared_"
                                                             "pointer_"
                                                             "lifecycle");

  auto channel = ChannelsRegistry::Global().getChannel("channel_shared_pointer_"
                                                       "lifecycle");

  channel->addDataSink(ros2_sink);

  double const value = 1.;
  channel->registerValue("value", &value);

  EXPECT_TRUE(channel->takeSnapshot());

  lifecycle_node->shutdown();
}

TEST(DataTamerROS2Publisher, Dereference)
{
  auto node = std::make_shared<rclcpp::Node>("test_datatamer_dereference");
  auto ros2_sink = ROS2PublisherSink::create(*node, "test_dereference");

  auto channel = ChannelsRegistry::Global().getChannel("channel_dereference");

  channel->addDataSink(ros2_sink);

  double const value = 1.;
  channel->registerValue("value", &value);

  EXPECT_TRUE(channel->takeSnapshot());
}

TEST(DataTamerROS2Publisher, DereferenceLifeCycle)
{
  auto lifecycle_node = std::make_shared<rclcpp_lifecycle::LifecycleNode>("test_"
                                                                          "datatamer_"
                                                                          "dereference_"
                                                                          "lifecycle");
  auto ros2_sink = ROS2PublisherSink::create(*lifecycle_node, "test_"
                                                              "dereference_"
                                                              "lifecycle");

  auto channel = ChannelsRegistry::Global().getChannel("channel_dereference_lifecycle");

  channel->addDataSink(ros2_sink);

  double const value = 1.;
  channel->registerValue("value", &value);

  EXPECT_TRUE(channel->takeSnapshot());

  lifecycle_node->shutdown();
}

TEST(DataTamerROS2Publisher, NodeInterfacesDirect)
{
  auto node = std::make_shared<rclcpp::Node>("test_datatamer_node_interfaces");
  PublisherNodeInterfaces interfaces(*node);
  auto ros2_sink = ROS2PublisherSink::create(interfaces, "test_node_"
                                                         "interfaces");

  auto channel = ChannelsRegistry::Global().getChannel("channel_node_interfaces");

  channel->addDataSink(ros2_sink);

  double const value = 1.;
  channel->registerValue("value", &value);

  EXPECT_TRUE(channel->takeSnapshot());
}

TEST(DataTamerROS2Publisher, NodeInterfacesDirectLifeCycle)
{
  auto lifecycle_node = std::make_shared<rclcpp_lifecycle::LifecycleNode>("test_"
                                                                          "datatamer_"
                                                                          "node_"
                                                                          "interfaces_"
                                                                          "lifecycle");
  PublisherNodeInterfaces interfaces(*lifecycle_node);
  auto ros2_sink = ROS2PublisherSink::create(interfaces, "test_node_interfaces_"
                                                         "lifecycle");

  auto channel = ChannelsRegistry::Global().getChannel("channel_node_interfaces_"
                                                       "lifecycle");

  channel->addDataSink(ros2_sink);

  double const value = 1.;
  channel->registerValue("value", &value);

  EXPECT_TRUE(channel->takeSnapshot());

  lifecycle_node->shutdown();
}

int main(int argc, char** argv)
{
  rclcpp::init(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  int ret = RUN_ALL_TESTS();
  rclcpp::shutdown();
  return ret;
}
