#include "data_tamer/channel.hpp"
#include "data_tamer/data_tamer.hpp"
#include "data_tamer/sinks/ros2_publisher_sink.hpp"

#include <cmath>
#include <string>

using namespace DataTamer;

// Usage: ros2_publisher [--aggregate] [--yaml]
//   --aggregate  publish SnapshotBatch messages on /test/data_batch (100 snapshots
//                or 100 ms per message) instead of one Snapshot per sample on /test/data
//   --yaml       write the schemas as YAML (wire format version 6)
// python/ros2_subscriber.py decodes either.
int main(int argc, char* argv[])
{
  rclcpp::init(argc, argv);
  auto node = std::make_shared<rclcpp::Node>("test_datatamer");

  ROS2PublisherOptions options;
  for(int i = 1; i < argc; i++)
  {
    const std::string arg = argv[i];
    options.aggregate |= (arg == "--aggregate");
    if(arg == "--yaml")
    {
      options.schema_format = SchemaFormat::Yaml;
    }
  }
  auto ros2_sink = ROS2PublisherSink::create(node, "test", options);
  ChannelsRegistry::Global().addDefaultSink(ros2_sink);

  // Create (or get) a channel using the global registry (singleton)
  auto channel = ChannelsRegistry::Global().getChannel("channel");

  auto const vect_size = 100;
  std::vector<double> real64(vect_size);
  std::vector<float> real32(vect_size);
  std::vector<int16_t> int16(vect_size);

  channel->registerValue("real64", &real64);
  channel->registerValue("real32", &real32);
  channel->registerValue("int16", &int16);

  int count = 0;
  double t = 0;

  while(rclcpp::ok())
  {
    double S = std::sin(t);
    for(size_t i = 0; i < vect_size; i++)
    {
      const double val = double(i) + S;
      real64[i] = val;
      real32[i] = float(val);
      int16[i] = int16_t(10 * (val));
    }
    if(count++ % 500 == 0)
    {
      RCLCPP_INFO(node->get_logger(), "snapshots: %d\n", count);
    }
    if(channel->takeSnapshot() != SnapshotResult::ok)
    {
      RCLCPP_ERROR(node->get_logger(), "pushing failed");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    t += 0.005;
  }
}
