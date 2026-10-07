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
  /// false (default): every snapshot is published as one
  /// data_tamer_msgs/Snapshot on `<topic_prefix>/data`.
  /// true: snapshots are aggregated into data_tamer_msgs/SnapshotBatch messages
  /// on `<topic_prefix>/data_batch`, and `<topic_prefix>/data` is not created.
  bool aggregate = false;
  /// Aggregation only: a batch is published as soon as it holds this many
  /// snapshots. 0 is treated as 1.
  size_t max_batch_size = 100;
  /// Aggregation only: a batch is also published when a snapshot arrives and
  /// at least this much time has passed since the first snapshot of the batch
  /// was added. Zero disables the time limit. There is no timer: a partial
  /// batch waits for the next snapshot, flush() or SinkWorker::stop().
  std::chrono::milliseconds max_batch_delay{ 100 };
  /// Aggregation only: copy into each batch the schemas of the snapshots it
  /// contains, so that a batch can be decoded on its own, without
  /// `<topic_prefix>/schemas` (which is published anyway).
  bool embed_schemas = true;
  /// Rendering of every `schema_text`, on `<topic_prefix>/schemas` and in
  /// embedded schemas. SchemaFormat::Yaml is shorter when field names are
  /// "/"-separated paths, but readers that predate it (e.g. older PlotJuggler
  /// releases) only understand SchemaFormat::Text.
  SchemaFormat schema_format = SchemaFormat::Text;
  /// QoS of `<topic_prefix>/data` (or `<topic_prefix>/data_batch`). The default
  /// is reliable with a history of the last 100 messages: a slow or stalled
  /// subscriber loses the oldest messages instead of making the publishing
  /// process queue messages without limit. Keep it reliable so that both
  /// reliable and best-effort subscribers match. With aggregation each message
  /// is a batch: size the depth accordingly. For a lossless stream, at the
  /// price of unbounded memory behind a slow subscriber, use
  /// `rclcpp::QoS(rclcpp::KeepAll()).reliable()`.
  /// `<topic_prefix>/schemas` is not affected: it is always reliable,
  /// transient-local, KeepLast(1).
  rclcpp::QoS data_qos = rclcpp::QoS(rclcpp::KeepLast(100)).reliable();
};

/// Publishes schemas and snapshots on `<topic_prefix>/schemas` and
/// `<topic_prefix>/data` (or `<topic_prefix>/data_batch`, see
/// ROS2PublisherOptions::aggregate). Create it with ROS2PublisherSink::create()
/// and pass the returned worker to LogChannel::addDataSink().
///
/// Every message on `<topic_prefix>/schemas` holds the complete catalog (all the
/// schemas the sink knows), and the topic is reliable, transient-local with
/// depth 1: a late subscriber receives the latest catalog. It is published as
/// soon as the sink learns a schema, i.e. by LogChannel::prepare() (explicit or
/// from the first takeSnapshot()) or by addDataSink() on a prepared channel, on
/// the calling thread. If that publish fails, prepare() still succeeds and the
/// catalog stays pending: the next snapshot retries it (a failure is counted by
/// the SinkWorker, see SinkWorker::errors()), and so does flush() (a failure
/// throws to its caller).
///
/// Call LogChannel::prepare() explicitly outside the control loop: otherwise the
/// first takeSnapshot() prepares the channel, which includes this DDS write.
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

  /// Publishes the pending batch, if any, and the schema catalog if its last
  /// publication failed (also without aggregation). Throws if a publication
  /// fails. SinkWorker::stop() calls it after the last delivery (a failure is
  /// then counted by SinkWorker::errors()); the destructor calls it too, as
  /// the fallback for a sink used without a worker.
  /// Thread-safe: may be called from any thread, e.g.
  /// `worker->as<ROS2PublisherSink>().flush()` after `worker->drain()`.
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
