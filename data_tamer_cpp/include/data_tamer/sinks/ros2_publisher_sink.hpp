#pragma once

#include "data_tamer/data_sink.hpp"
#include "data_tamer/types.hpp"

#include <chrono>
#include <cstddef>
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

struct ROS2PublisherOptions
{
  /// false: one data_tamer_msgs/Snapshot per snapshot on `<topic_prefix>/data`.
  /// true: data_tamer_msgs/SnapshotBatch messages on `<topic_prefix>/data_batch`
  /// instead.
  bool aggregate = false;
  /// Aggregation only: publish a batch once it holds this many snapshots (0 means 1).
  size_t max_batch_size = 100;
  /// Aggregation only: also publish when a snapshot arrives and the batch's first
  /// snapshot is at least this old (steady clock). 0 disables the limit. There is no
  /// timer: a partial batch waits for the next snapshot, flush() or SinkWorker::stop().
  std::chrono::milliseconds max_batch_delay{ 100 };
  /// Aggregation only: put the schemas of a batch's snapshots into the batch, so that
  /// it decodes on its own.
  bool embed_schemas = true;
  /// Rendering of every `schema_text`. SchemaFormat::Yaml is shorter, but older readers
  /// (e.g. older PlotJuggler releases) only understand SchemaFormat::Text.
  SchemaFormat schema_format = SchemaFormat::Text;
  /// QoS of the data topic. The default, reliable KeepLast(100), bounds the publisher's
  /// memory: a slow subscriber loses the oldest messages. Keep it reliable so that
  /// reliable and best-effort subscribers both match, and size the depth in batches when
  /// aggregating. KeepAll is lossless but grows without bound behind a slow subscriber.
  /// It does not apply to the `schemas` topic.
  rclcpp::QoS data_qos = rclcpp::QoS(rclcpp::KeepLast(100)).reliable();
};

/// Publishes the schemas on `<topic_prefix>/schemas` and the snapshots on
/// `<topic_prefix>/data` (or `<topic_prefix>/data_batch`, see
/// ROS2PublisherOptions::aggregate). Create it with ROS2PublisherSink::create() and
/// attach the returned worker with LogChannel::addDataSink().
///
/// Each `schemas` message is the complete catalog (reliable, transient-local, depth 1),
/// so late subscribers get it. It is published from the thread that calls startLogging()
/// or addDataSink(), so call startLogging() before the control loop. A failed publish is
/// retried by the next snapshot and by flush().
class ROS2PublisherSink : public DataSink
{
public:
  template <typename NodeT>
  ROS2PublisherSink(NodeT&& nodelike, const std::string& topic_prefix,
                    const ROS2PublisherOptions& options = {})
    : ROS2PublisherSink(normalize_node(std::forward<NodeT>(nodelike)), topic_prefix,
                        options, ConstructorTag{})
  {}

  template <typename NodeT>
  static std::shared_ptr<SinkWorker> create(NodeT&& nodelike,
                                            const std::string& topic_prefix,
                                            const ROS2PublisherOptions& options = {})
  {
    return SinkWorker::create<ROS2PublisherSink>(std::forward<NodeT>(nodelike),
                                                 topic_prefix, options);
  }

  /// Publish the pending batch, if any, and the schema catalog if its last publication
  /// failed. Throws if a publication fails. SinkWorker::stop() calls it after the last
  /// delivery (a throw is then counted in SinkWorker::errors()). Callable from any
  /// thread, e.g. `worker->as<ROS2PublisherSink>().flush()` after `worker->drain()`.
  void flush();

  ~ROS2PublisherSink() override;

protected:
  void onSchema(const Schema& schema) override;
  void onSnapshot(const SnapshotRef& snapshot) override;
  /// Publishes the partial batch, as flush().
  void onStop() override;

private:
  struct Pimpl;
  std::unique_ptr<Pimpl> _p;

  struct ConstructorTag
  {
  };

  ROS2PublisherSink(PublisherNodeInterfaces node_interface,
                    const std::string& topic_prefix, const ROS2PublisherOptions& options,
                    ConstructorTag);

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
        "`ROS2PublisherSink(NodeT&& nodelike, const std::string& topic_prefix, ...)`. "
        "Pass a "
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
