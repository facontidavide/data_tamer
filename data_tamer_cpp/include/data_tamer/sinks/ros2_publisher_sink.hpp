#pragma once

#include "data_tamer/data_sink.hpp"

#include <memory>
#include <string>
#include <type_traits>
#include <utility>

#include <rclcpp/rclcpp.hpp>
#include <rclcpp/node_interfaces/node_interfaces.hpp>
#include <rclcpp/node_interfaces/node_topics_interface.hpp>
#include <rclcpp_lifecycle/lifecycle_node.hpp>

namespace DataTamer
{

using PublisherNodeInterfaces =
    rclcpp::node_interfaces::NodeInterfaces<rclcpp::node_interfaces::NodeTopicsInterface>;

/// Publishes schemas and snapshots on `<topic_prefix>/schemas` and
/// `<topic_prefix>/data`. Create it with ROS2PublisherSink::create() and pass
/// the returned worker to LogChannel::addDataSink().
class ROS2PublisherSink : public DataSink
{
public:
  template <typename NodeT>
  ROS2PublisherSink(NodeT&& nodelike, const std::string& topic_prefix)
    : ROS2PublisherSink(normalize_node(std::forward<NodeT>(nodelike)), topic_prefix,
                        ConstructorTag{})
  {}

  template <typename NodeT>
  static std::shared_ptr<SinkWorker> create(NodeT&& nodelike,
                                            const std::string& topic_prefix)
  {
    return SinkWorker::create<ROS2PublisherSink>(std::forward<NodeT>(nodelike),
                                                 topic_prefix);
  }

  ~ROS2PublisherSink() override;

protected:
  void onSchema(const Schema& schema) override;
  void onSnapshot(const SnapshotRef& snapshot) override;

private:
  struct Pimpl;
  std::unique_ptr<Pimpl> _p;

  struct ConstructorTag
  {
  };

  ROS2PublisherSink(PublisherNodeInterfaces node_interface,
                    const std::string& topic_prefix, ConstructorTag);

  template <typename NodeT>
  static PublisherNodeInterfaces normalize_node(NodeT&& nodelike)
  {
    using D = std::decay_t<NodeT>;

    // use a friendlier compile error than the one that would otherwise come out
    static_assert(
        std::is_same_v<D, PublisherNodeInterfaces> ||
            std::is_same_v<D, std::shared_ptr<rclcpp::Node>> ||
            std::is_same_v<D, std::shared_ptr<rclcpp_lifecycle::LifecycleNode>> ||
            std::is_constructible_v<PublisherNodeInterfaces, D&>,
        "ROS2PublisherSink: unsupported node-like type passed to "
        "`ROS2PublisherSink(NodeT&& nodelike, const std::string& topic_prefix)`. Pass a "
        "rclcpp::Node, "
        "rclcpp_lifecycle::LifecycleNode, a shared_ptr to either, or a "
        "PublisherNodeInterfaces.");

    if constexpr(std::is_same_v<D, PublisherNodeInterfaces>)
    {
      return nodelike;
    }
    else if constexpr(std::is_same_v<D, std::shared_ptr<rclcpp::Node>> ||
                      std::is_same_v<D, std::shared_ptr<rclcpp_lifecycle::LifecycleNode>>)
    {
      return PublisherNodeInterfaces(*nodelike);
    }
    else
    {
      return PublisherNodeInterfaces(nodelike);
    }
  }

  void create_publishers(const std::string& topic_prefix);
};

}  // namespace DataTamer
