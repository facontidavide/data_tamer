#include "data_tamer/sinks/ros2_publisher_sink.hpp"
#include "data_tamer_msgs/msg/schemas.hpp"
#include "data_tamer_msgs/msg/snapshot.hpp"
#include "data_tamer_msgs/msg/snapshot_batch.hpp"

#include <algorithm>
#include <iterator>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace DataTamer
{

namespace
{
void FillSnapshotMsg(const Snapshot& snapshot, data_tamer_msgs::msg::Snapshot& msg)
{
  msg.timestamp_nsec = uint64_t(snapshot.timestamp.count());
  msg.schema_hash = snapshot.schema_hash;
  msg.active_mask = snapshot.active_mask;
  msg.payload = snapshot.payload;
}
}  // namespace

struct ROS2PublisherSink::Pimpl
{
  Pimpl(PublisherNodeInterfaces interfaces, const ROS2PublisherOptions& opts)
    : options(opts), node_interface(std::move(interfaces))
  {
    options.max_batch_size = std::max<size_t>(1, opts.max_batch_size);
  }

  // onSchema/onSnapshot are serialized by the SinkWorker, but flush() may be
  // called from any thread.
  std::mutex mutex;

  ROS2PublisherOptions options;

  // Schemas in message form: the text is serialized once, in onSchema().
  std::unordered_map<uint64_t, data_tamer_msgs::msg::Schema> schemas;

  rclcpp::Publisher<data_tamer_msgs::msg::Schemas>::SharedPtr schema_publisher;
  rclcpp::Publisher<data_tamer_msgs::msg::Snapshot>::SharedPtr data_publisher;
  rclcpp::Publisher<data_tamer_msgs::msg::SnapshotBatch>::SharedPtr batch_publisher;

  // The catalog changed and has not been published successfully since.
  bool schema_changed = false;
  data_tamer_msgs::msg::Snapshot data_msg;

  data_tamer_msgs::msg::SnapshotBatch batch_msg;
  // Snapshots of the current batch: the first batch_count elements of
  // batch_msg.snapshots. Elements are reused across batches, so that copying a
  // snapshot into them does not allocate once their buffers have grown.
  size_t batch_count = 0;
  // Elements past batch_count, set aside while a batch closed early (flush(),
  // max_batch_delay) is published, so that their buffers are not freed.
  std::vector<data_tamer_msgs::msg::Snapshot> spare_snapshots;
  std::chrono::steady_clock::time_point batch_start;

  PublisherNodeInterfaces node_interface;

  void publishSchemasIfChanged();
  void addToBatch(const Snapshot& snapshot);
  void publishBatch();
};

ROS2PublisherSink::ROS2PublisherSink(PublisherNodeInterfaces node_interface,
                                     const std::string& topic_prefix,
                                     const ROS2PublisherOptions& options, ConstructorTag)
  : _p(std::make_unique<Pimpl>(std::move(node_interface), options))
{
  create_publishers(topic_prefix);
}

ROS2PublisherSink::~ROS2PublisherSink()
{
  // A sink owned by a SinkWorker was flushed by onStop() already: this flush
  // is the fallback for a sink used without a worker (and retries what a
  // failed onStop() left). The worker is stopped before the sink is
  // destroyed: no concurrent callback.
  try
  {
    flush();
  }
  catch(...)
  {
    // e.g. the ROS context was already shut down; nothing left to report to.
  }
}

void ROS2PublisherSink::create_publishers(const std::string& topic_prefix)
{
  // Every message is the complete catalog: keeping the latest one is enough for
  // late subscribers (transient-local), and bounds what the publisher retains.
  rclcpp::QoS schemas_qos{ rclcpp::KeepLast(1) };
  schemas_qos.reliable();
  schemas_qos.transient_local();  // latch

  const rclcpp::QoS& data_qos = _p->options.data_qos;

  _p->schema_publisher = rclcpp::create_publisher<data_tamer_msgs::msg::Schemas>(
      _p->node_interface, topic_prefix + "/schemas", schemas_qos);
  if(_p->options.aggregate)
  {
    _p->batch_publisher = rclcpp::create_publisher<data_tamer_msgs::msg::SnapshotBatch>(
        _p->node_interface, topic_prefix + "/data_batch", data_qos);
    const size_t reserved = std::min<size_t>(_p->options.max_batch_size, 1024);
    _p->batch_msg.snapshots.reserve(reserved);
    _p->spare_snapshots.reserve(reserved);
  }
  else
  {
    _p->data_publisher = rclcpp::create_publisher<data_tamer_msgs::msg::Snapshot>(
        _p->node_interface, topic_prefix + "/data", data_qos);
  }
}

void ROS2PublisherSink::onSchema(const Schema& schema)
{
  data_tamer_msgs::msg::Schema schema_msg;
  schema_msg.hash = schema.hash;
  schema_msg.channel_name = schema.channel_name;
  schema_msg.schema_text = RenderSchema(schema, _p->options.schema_format);

  std::lock_guard lock(_p->mutex);
  _p->schemas[schema.hash] = std::move(schema_msg);
  _p->schema_changed = true;

  // Publish the catalog now, from the thread running prepare() / addDataSink()
  // (serialized with onSnapshot() by the SinkWorker), so that subscribers get
  // it without waiting for a snapshot. Publisher::publish() is thread-safe.
  // A failure must not make prepare() fail: the catalog stays pending, and the
  // next onSnapshot() or flush() retries it and reports the error.
  try
  {
    _p->publishSchemasIfChanged();
  }
  catch(...)
  {}
}

void ROS2PublisherSink::onSnapshot(const SnapshotRef& ref)
{
  std::lock_guard lock(_p->mutex);
  _p->publishSchemasIfChanged();

  const Snapshot& snapshot = *ref;
  if(_p->options.aggregate)
  {
    _p->addToBatch(snapshot);
    return;
  }
  FillSnapshotMsg(snapshot, _p->data_msg);
  _p->data_publisher->publish(_p->data_msg);
}

void ROS2PublisherSink::onStop()
{
  flush();
}

void ROS2PublisherSink::flush()
{
  std::lock_guard lock(_p->mutex);
  _p->publishSchemasIfChanged();  // a catalog whose publication failed earlier
  if(_p->options.aggregate && _p->batch_count > 0)
  {
    _p->publishBatch();
  }
}

void ROS2PublisherSink::Pimpl::publishSchemasIfChanged()
{
  if(!schema_changed)
  {
    return;
  }
  data_tamer_msgs::msg::Schemas msg;
  msg.schemas.reserve(schemas.size());
  for(const auto& entry : schemas)
  {
    msg.schemas.push_back(entry.second);
  }
  schema_publisher->publish(msg);
  schema_changed = false;  // only once published; a throw leaves it pending
}

void ROS2PublisherSink::Pimpl::addToBatch(const Snapshot& snapshot)
{
  const auto now = std::chrono::steady_clock::now();
  auto& snapshots = batch_msg.snapshots;
  if(batch_count == 0)
  {
    batch_start = now;
  }
  if(batch_count == snapshots.size())
  {
    snapshots.emplace_back();
  }
  FillSnapshotMsg(snapshot, snapshots[batch_count++]);

  if(options.embed_schemas)
  {
    auto& embedded = batch_msg.schemas;
    const bool present =
        std::any_of(embedded.begin(), embedded.end(),
                    [&](const auto& s) { return s.hash == snapshot.schema_hash; });
    if(!present)
    {
      if(auto it = schemas.find(snapshot.schema_hash); it != schemas.end())
      {
        embedded.push_back(it->second);
      }
    }
  }

  const bool full = batch_count >= options.max_batch_size;
  const bool expired = options.max_batch_delay.count() > 0 &&
                       (now - batch_start) >= options.max_batch_delay;
  if(full || expired)
  {
    publishBatch();
  }
}

void ROS2PublisherSink::Pimpl::publishBatch()
{
  // Reset the batch even if publish() throws, so that a failing publisher
  // drops one batch instead of growing it without bound.
  struct Clear
  {
    Pimpl& p;
    ~Clear()
    {
      p.batch_count = 0;
      p.batch_msg.schemas.clear();
      for(auto& spare : p.spare_snapshots)
      {
        p.batch_msg.snapshots.push_back(std::move(spare));
      }
      p.spare_snapshots.clear();
    }
  } clear{ *this };
  auto& snapshots = batch_msg.snapshots;
  const auto unused = snapshots.begin() + static_cast<std::ptrdiff_t>(batch_count);
  spare_snapshots.assign(std::make_move_iterator(unused),
                         std::make_move_iterator(snapshots.end()));
  snapshots.erase(unused, snapshots.end());
  batch_publisher->publish(batch_msg);
}

}  // namespace DataTamer
