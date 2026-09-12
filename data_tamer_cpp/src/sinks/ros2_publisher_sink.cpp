#include "data_tamer/sinks/ros2_publisher_sink.hpp"
#include "data_tamer_msgs/msg/schemas.hpp"
#include "data_tamer_msgs/msg/snapshot.hpp"

#include <mutex>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace DataTamer
{

struct ROS2PublisherSink::Pimpl
{
  explicit Pimpl(PublisherNodeInterfaces node_interface)
    : node_interface(std::move(node_interface))
  {}

  std::unordered_map<std::string, Schema> schemas;
  std::mutex schema_mutex;

  rclcpp::Publisher<data_tamer_msgs::msg::Schemas>::SharedPtr schema_publisher;
  rclcpp::Publisher<data_tamer_msgs::msg::Snapshot>::SharedPtr data_publisher;

  bool schema_changed = true;
  data_tamer_msgs::msg::Snapshot data_msg;
  PublisherNodeInterfaces node_interface;
};

ROS2PublisherSink::ROS2PublisherSink(PublisherNodeInterfaces node_interface,
                                     const std::string& topic_prefix, ConstructorTag)
  : _p(std::make_unique<Pimpl>(std::move(node_interface)))
{
  create_publishers(topic_prefix);
}

ROS2PublisherSink::~ROS2PublisherSink()
{
  stopThread();
}

void ROS2PublisherSink::create_publishers(const std::string& topic_prefix)
{
  rclcpp::QoS schemas_qos{ rclcpp::KeepAll() };
  schemas_qos.reliable();
  schemas_qos.transient_local();  // latch

  const rclcpp::QoS data_qos{ rclcpp::KeepAll() };

  _p->schema_publisher = rclcpp::create_publisher<data_tamer_msgs::msg::Schemas>(
      _p->node_interface, topic_prefix + "/schemas", schemas_qos);
  _p->data_publisher = rclcpp::create_publisher<data_tamer_msgs::msg::Snapshot>(
      _p->node_interface, topic_prefix + "/data", data_qos);
}

void ROS2PublisherSink::addChannel(const std::string& channel_name, const Schema& schema)
{
  std::scoped_lock lk(_p->schema_mutex);
  _p->schemas[channel_name] = schema;
  _p->schema_changed = true;
}

bool ROS2PublisherSink::storeSnapshot(const Snapshot& snapshot)
{
  // send the schemas, if you haven't yet.
  {
    std::scoped_lock lk(_p->schema_mutex);
    if(_p->schema_changed)
    {
      _p->schema_changed = false;
      data_tamer_msgs::msg::Schemas msg;
      msg.schemas.reserve(_p->schemas.size());

      for(const auto& [channel_name, schema] : _p->schemas)
      {
        data_tamer_msgs::msg::Schema schema_msg;
        schema_msg.hash = schema.hash;
        schema_msg.channel_name = channel_name;
        std::ostringstream ss;
        ss << schema;
        schema_msg.schema_text = ss.str();

        msg.schemas.push_back(std::move(schema_msg));
      }
      _p->schema_publisher->publish(msg);
    }
  }
  //----------------------------------------
  _p->data_msg.timestamp_nsec = uint64_t(snapshot.timestamp.count());
  _p->data_msg.schema_hash = snapshot.schema_hash;
  _p->data_msg.active_mask = snapshot.active_mask;
  _p->data_msg.payload = snapshot.payload;
  _p->data_publisher->publish(_p->data_msg);

  return true;
}

}  // namespace DataTamer
