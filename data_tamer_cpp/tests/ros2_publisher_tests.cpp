#include "data_tamer/data_tamer.hpp"
#include "data_tamer/sinks/dummy_sink.hpp"
#include "data_tamer/sinks/ros2_publisher_sink.hpp"

#include "data_tamer_msgs/msg/schemas.hpp"
#include "data_tamer_msgs/msg/snapshot_batch.hpp"
#include "data_tamer_parser/data_tamer_parser.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <set>
#include <thread>
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

  EXPECT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
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

  EXPECT_EQ(channel->takeSnapshot(), SnapshotResult::ok);

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

  EXPECT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
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

  EXPECT_EQ(channel->takeSnapshot(), SnapshotResult::ok);

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

  EXPECT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
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

  EXPECT_EQ(channel->takeSnapshot(), SnapshotResult::ok);

  lifecycle_node->shutdown();
}

namespace
{
using BatchMsg = data_tamer_msgs::msg::SnapshotBatch;

// Calls `produce` and spins until a batch arrives on `<prefix>/data_batch`.
// Retries because a publisher drops messages sent before it matched the subscriber.
template <typename Produce>
std::optional<BatchMsg> receiveBatch(const std::shared_ptr<rclcpp::Node>& node,
                                     const std::string& prefix, Produce&& produce)
{
  std::optional<BatchMsg> received;
  auto sub = node->create_subscription<BatchMsg>(
      prefix + "/data_batch", rclcpp::QoS(rclcpp::KeepAll()), [&](const BatchMsg& msg) {
        if(!received)
        {
          received = msg;
        }
      });
  rclcpp::executors::SingleThreadedExecutor executor;
  executor.add_node(node);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while(!received && std::chrono::steady_clock::now() < deadline)
  {
    produce();
    executor.spin_some(std::chrono::milliseconds(100));
  }
  return received;
}
}  // namespace

TEST(DataTamerROS2Publisher, AggregateBySize)
{
  auto node = std::make_shared<rclcpp::Node>("test_datatamer_aggregate_size");
  ROS2PublisherOptions options;
  options.aggregate = true;
  options.max_batch_size = 5;
  options.max_batch_delay = std::chrono::milliseconds(0);
  auto ros2_sink = ROS2PublisherSink::create(node, "test_aggregate_size", options);

  auto channel = ChannelsRegistry::Global().getChannel("channel_aggregate_size");
  channel->addDataSink(ros2_sink);
  double value = 1.;
  channel->registerValue("value", &value);

  auto batch = receiveBatch(node, "test_aggregate_size", [&] {
    for(int i = 0; i < 5; i++)
    {
      value += 1.0;
      ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
    }
    ros2_sink->drain();
  });

  ASSERT_TRUE(batch.has_value());
  ASSERT_EQ(batch->snapshots.size(), 5u);
  ASSERT_EQ(batch->schemas.size(), 1u);
  EXPECT_EQ(batch->schemas[0].channel_name, "channel_aggregate_size");
  EXPECT_EQ(batch->schemas[0].hash, channel->getSchema().hash);
  EXPECT_FALSE(batch->schemas[0].schema_text.empty());
  for(const auto& snapshot : batch->snapshots)
  {
    EXPECT_EQ(snapshot.schema_hash, channel->getSchema().hash);
    EXPECT_EQ(snapshot.payload.size(), sizeof(double));
  }

  // decode with the parser helpers: the batch is self-contained
  DataTamerParser::SchemaRegistry registry;
  std::vector<double> values;
  const size_t visited = DataTamerParser::ForEachSnapshotInBatch(
      registry, *batch,
      [&](const DataTamerParser::Schema& schema,
          const DataTamerParser::SnapshotView& view) {
        EXPECT_TRUE(DataTamerParser::ParseSnapshot(
            schema, view,
            [&](const std::string& name, const DataTamerParser::VarNumber& n) {
              EXPECT_EQ(name, "value");
              values.push_back(std::get<double>(n));
            }));
      });
  EXPECT_EQ(visited, 5u);
  ASSERT_EQ(values.size(), 5u);
  for(size_t i = 1; i < values.size(); i++)
  {
    EXPECT_EQ(values[i], values[i - 1] + 1.0);
  }
}

TEST(DataTamerROS2Publisher, AggregateFlushWithoutSchemas)
{
  auto node = std::make_shared<rclcpp::Node>("test_datatamer_aggregate_flush");
  ROS2PublisherOptions options;
  options.aggregate = true;
  options.max_batch_size = 1000;
  options.max_batch_delay = std::chrono::milliseconds(0);
  options.embed_schemas = false;
  auto ros2_sink = ROS2PublisherSink::create(node, "test_aggregate_flush", options);

  auto channel = ChannelsRegistry::Global().getChannel("channel_aggregate_flush");
  channel->addDataSink(ros2_sink);
  double const value = 1.;
  channel->registerValue("value", &value);

  auto batch = receiveBatch(node, "test_aggregate_flush", [&] {
    for(int i = 0; i < 3; i++)
    {
      ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
    }
    ros2_sink->drain();
    ros2_sink->as<ROS2PublisherSink>().flush();
  });

  ASSERT_TRUE(batch.has_value());
  EXPECT_EQ(batch->snapshots.size(), 3u);
  EXPECT_TRUE(batch->schemas.empty());
}

TEST(DataTamerROS2Publisher, AggregateByDelay)
{
  auto node = std::make_shared<rclcpp::Node>("test_datatamer_aggregate_delay");
  ROS2PublisherOptions options;
  options.aggregate = true;
  options.max_batch_size = 1000;
  options.max_batch_delay = std::chrono::milliseconds(5);
  auto ros2_sink = ROS2PublisherSink::create(node, "test_aggregate_delay", options);

  auto channel = ChannelsRegistry::Global().getChannel("channel_aggregate_delay");
  channel->addDataSink(ros2_sink);
  double const value = 1.;
  channel->registerValue("value", &value);

  // the second snapshot arrives after the delay: it closes a batch of two
  auto batch = receiveBatch(node, "test_aggregate_delay", [&] {
    ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
    ros2_sink->drain();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
    ros2_sink->drain();
  });
  ASSERT_TRUE(batch.has_value());
  EXPECT_EQ(batch->snapshots.size(), 2u);
}

TEST(DataTamerROS2Publisher, DestructorPublishesPendingBatch)
{
  auto node = std::make_shared<rclcpp::Node>("test_datatamer_aggregate_destructor");
  ROS2PublisherOptions options;
  options.aggregate = true;
  options.max_batch_size = 1000;
  options.max_batch_delay = std::chrono::milliseconds(0);

  auto batch = receiveBatch(node, "test_aggregate_destructor", [&] {
    // a new sink (and publisher) per attempt; give it time to match the subscriber
    auto ros2_sink =
        ROS2PublisherSink::create(node, "test_aggregate_destructor", options);
    auto channel = LogChannel::create("channel_aggregate_destructor");
    channel->addDataSink(ros2_sink);
    double const value = 1.;
    channel->registerValue("value", &value);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    for(int i = 0; i < 3; i++)
    {
      ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
    }
    ros2_sink->drain();
    channel->removeDataSink(ros2_sink);
    ros2_sink.reset();  // the last owner: the sink flushes in its destructor
  });
  ASSERT_TRUE(batch.has_value());
  EXPECT_EQ(batch->snapshots.size(), 3u);
}

TEST(DataTamerROS2Publisher, AggregateWithYamlSchemas)
{
  auto node = std::make_shared<rclcpp::Node>("test_datatamer_aggregate_yaml");
  ROS2PublisherOptions options;
  options.aggregate = true;
  options.max_batch_size = 2;
  options.schema_format = SchemaFormat::Yaml;
  auto ros2_sink = ROS2PublisherSink::create(node, "test_aggregate_yaml", options);

  auto channel = ChannelsRegistry::Global().getChannel("channel_aggregate_yaml");
  channel->addDataSink(ros2_sink);
  double const value = 3.;
  channel->registerValue("robot/arm/value", &value);
  channel->registerValue("robot/arm/other", &value);

  auto batch = receiveBatch(node, "test_aggregate_yaml", [&] {
    for(int i = 0; i < 2; i++)
    {
      ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
    }
    ros2_sink->drain();
  });

  ASSERT_TRUE(batch.has_value());
  ASSERT_EQ(batch->schemas.size(), 1u);
  EXPECT_EQ(batch->schemas[0].schema_text, ToYaml(channel->getSchema()));

  DataTamerParser::SchemaRegistry registry;
  std::vector<std::string> names;
  EXPECT_EQ(DataTamerParser::ForEachSnapshotInBatch(
                registry, *batch,
                [&](const DataTamerParser::Schema& schema,
                    const DataTamerParser::SnapshotView& view) {
                  DataTamerParser::ParseSnapshot(
                      schema, view,
                      [&](const std::string& name, const DataTamerParser::VarNumber&) {
                        names.push_back(name);
                      });
                }),
            2u);
  EXPECT_EQ(names.size(), 4u);
  EXPECT_EQ(names.front(), "robot/arm/value");
}

namespace
{
// Subscribes late to `<prefix>/schemas` with a KeepAll transient-local reader, so
// that every sample the writer retains is delivered, and collects messages
// until 300 ms after the first one.
std::vector<data_tamer_msgs::msg::Schemas> receiveRetainedSchemas(
    const std::shared_ptr<rclcpp::Node>& node, const std::string& prefix)
{
  rclcpp::QoS latched{ rclcpp::KeepAll() };
  latched.reliable();
  latched.transient_local();
  std::vector<data_tamer_msgs::msg::Schemas> received;
  auto sub = node->create_subscription<data_tamer_msgs::msg::Schemas>(
      prefix + "/schemas", latched,
      [&](const data_tamer_msgs::msg::Schemas& msg) { received.push_back(msg); });

  rclcpp::executors::SingleThreadedExecutor executor;
  executor.add_node(node);
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  bool first_seen = false;
  while(std::chrono::steady_clock::now() < deadline)
  {
    executor.spin_some(std::chrono::milliseconds(20));
    if(!first_seen && !received.empty())
    {
      first_seen = true;
      deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    }
  }
  return received;
}

std::set<uint64_t> hashesOf(const data_tamer_msgs::msg::Schemas& msg)
{
  std::set<uint64_t> hashes;
  for(const auto& schema : msg.schemas)
  {
    hashes.insert(schema.hash);
    EXPECT_FALSE(schema.schema_text.empty());
  }
  return hashes;
}
}  // namespace

TEST(DataTamerROS2Publisher, SchemasPublishedOnPrepareForLateJoiners)
{
  auto node = std::make_shared<rclcpp::Node>("test_datatamer_schemas_late");
  auto ros2_sink = ROS2PublisherSink::create(node, "test_schemas_late");

  // two channels, prepared without taking any snapshot: two catalogs published
  auto channel_a = LogChannel::create("channel_schemas_late_a");
  auto channel_b = LogChannel::create("channel_schemas_late_b");
  double const value = 1.;
  channel_a->registerValue("a", &value);
  channel_b->registerValue("b", &value);
  channel_a->addDataSink(ros2_sink);
  channel_b->addDataSink(ros2_sink);
  channel_a->prepare();
  channel_b->prepare();

  // the writer keeps only the latest one, which is the complete catalog
  const auto received = receiveRetainedSchemas(node, "test_schemas_late");
  ASSERT_EQ(received.size(), 1u);
  EXPECT_EQ(hashesOf(received[0]), (std::set<uint64_t>{ channel_a->getSchema().hash,
                                                        channel_b->getSchema().hash }));
}

TEST(DataTamerROS2Publisher, SchemasPublishedOnAddDataSinkToPreparedChannel)
{
  auto node = std::make_shared<rclcpp::Node>("test_datatamer_schemas_add_sink");
  auto ros2_sink = ROS2PublisherSink::create(node, "test_schemas_add_sink");

  auto channel = LogChannel::create("channel_schemas_add_sink");
  double const value = 1.;
  channel->registerValue("value", &value);
  channel->addDataSink(DummySink::create());
  channel->prepare();
  channel->addDataSink(ros2_sink);  // announced now; no snapshot is taken

  const auto received = receiveRetainedSchemas(node, "test_schemas_add_sink");
  ASSERT_EQ(received.size(), 1u);
  EXPECT_EQ(hashesOf(received[0]), (std::set<uint64_t>{ channel->getSchema().hash }));
}

TEST(DataTamerROS2Publisher, QoS)
{
  auto node = std::make_shared<rclcpp::Node>("test_datatamer_qos");
  ROS2PublisherOptions options;
  options.data_qos = rclcpp::QoS(rclcpp::KeepLast(5)).best_effort();
  auto ros2_sink = ROS2PublisherSink::create(node, "test_qos", options);

  // the depth is not part of discovery data: check the policies that are
  auto info_of = [&](const std::string& topic) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    auto info = node->get_publishers_info_by_topic(topic);
    while(info.empty() && std::chrono::steady_clock::now() < deadline)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      info = node->get_publishers_info_by_topic(topic);
    }
    return info;
  };
  auto data_info = info_of("test_qos/data");
  ASSERT_EQ(data_info.size(), 1u);
  EXPECT_EQ(data_info[0].qos_profile().reliability(),
            rclcpp::ReliabilityPolicy::BestEffort);
  EXPECT_EQ(data_info[0].qos_profile().durability(), rclcpp::DurabilityPolicy::Volatile);

  auto schemas_info = info_of("test_qos/schemas");
  ASSERT_EQ(schemas_info.size(), 1u);
  EXPECT_EQ(schemas_info[0].qos_profile().reliability(),
            rclcpp::ReliabilityPolicy::Reliable);
  EXPECT_EQ(schemas_info[0].qos_profile().durability(),
            rclcpp::DurabilityPolicy::TransientLocal);

  // default data QoS: reliable, bounded history
  const ROS2PublisherOptions defaults;
  EXPECT_EQ(defaults.data_qos.reliability(), rclcpp::ReliabilityPolicy::Reliable);
  EXPECT_EQ(defaults.data_qos.history(), rclcpp::HistoryPolicy::KeepLast);
  EXPECT_EQ(defaults.data_qos.depth(), 100u);
}

int main(int argc, char** argv)
{
  rclcpp::init(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  int ret = RUN_ALL_TESTS();
  rclcpp::shutdown();
  return ret;
}
