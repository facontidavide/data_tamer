#pragma once

#include "data_tamer/types.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <typeinfo>
#include <vector>

namespace moodycamel
{
struct ProducerToken;
}

namespace DataTamer
{

class LogChannel;
class SinkWorker;
class SnapshotPool;
struct PoolSlot;

using ActiveMask = std::vector<uint8_t>;
using PayloadVector = std::vector<uint8_t>;

bool GetBit(const ActiveMask& mask, size_t index);
void SetBit(ActiveMask& mask, size_t index, bool val);

/// One captured sample of a channel. Fully owning: a copy is an independent record.
/// The channel is identified by schema_hash (the hash covers the channel name).
struct Snapshot
{
  /// Unique identifier of the schema
  uint64_t schema_hash;
  /// snapshot timestamp
  std::chrono::nanoseconds timestamp;
  /// Vector that tell us if a field of the schema is
  /// active or not. It is basically an optimized vector
  /// of bools, where each byte contains 8 boolean flags.
  ActiveMask active_mask;
  /// serialized dat containing all the values, ordered as in the schema
  PayloadVector payload;
};

/**
 * @brief Move-only handle to a pooled Snapshot. Holds one reference on the
 * pool slot, so a sink may keep it for as long as it likes: the slot is not
 * reused until the last handle is destroyed, even if the channel is gone.
 * clone() adds a reference without copying the data; `Snapshot copy = *ref;`
 * makes an independent copy.
 */
class SnapshotRef
{
public:
  SnapshotRef() = default;
  /// Takes ownership of one already-counted reference on `slot` (library internal).
  SnapshotRef(std::shared_ptr<SnapshotPool> pool, PoolSlot* slot);
  SnapshotRef(SnapshotRef&& other) noexcept;
  SnapshotRef& operator=(SnapshotRef&& other) noexcept;
  SnapshotRef(const SnapshotRef&) = delete;
  SnapshotRef& operator=(const SnapshotRef&) = delete;
  ~SnapshotRef();

  /// Explicit share: adds a reference to the same slot.
  [[nodiscard]] SnapshotRef clone() const;
  void reset();

  const Snapshot& operator*() const;
  const Snapshot* operator->() const;
  explicit operator bool() const { return slot_ != nullptr; }

private:
  std::shared_ptr<SnapshotPool> pool_;
  PoolSlot* slot_ = nullptr;
};

/**
 * @brief Interface implemented by a sink. The two callbacks are invoked only by
 * the SinkWorker that owns the sink and are serialized with each other, so a
 * sink needs no lock of its own for the state they touch:
 *
 * - onSchema() runs on the thread that attaches the channel to the sink
 *   (addDataSink or the first takeSnapshot), once per channel schema.
 * - onSnapshot() runs on the worker thread, in queue order. Throw to report a
 *   failure: the worker counts it and keeps the message (see SinkWorker).
 *
 * Never call LogChannel control methods from a callback.
 */
class DataSink
{
public:
  virtual ~DataSink() = default;

protected:
  friend class SinkWorker;
  virtual void onSchema(const Schema& schema) = 0;
  virtual void onSnapshot(const SnapshotRef& snapshot) = 0;
};

/**
 * @brief Owns a DataSink, the queue that channels publish into and the thread
 * that delivers queued snapshots to the sink. It is what LogChannel::addDataSink
 * takes. The worker is stopped before the sink is destroyed, so a sink never
 * receives a callback while it is being torn down.
 */
class SinkWorker
{
public:
  static constexpr size_t kDefaultQueueCapacity = 1024;

  enum class Delivery : uint8_t
  {
    /// A worker thread delivers queued snapshots as they arrive (default).
    Threaded,
    /// No thread: the application delivers by calling drain() itself.
    Manual
  };

  /// `queue_capacity` is preallocated (block-rounded) and shared by all producers.
  explicit SinkWorker(std::unique_ptr<DataSink> sink,
                      size_t queue_capacity = kDefaultQueueCapacity,
                      Delivery delivery = Delivery::Threaded);
  ~SinkWorker();
  SinkWorker(const SinkWorker&) = delete;
  SinkWorker& operator=(const SinkWorker&) = delete;

  /// Build a sink of type T and wrap it: SinkWorker::create<MCAPSink>("out.mcap").
  template <typename T, typename... Args>
  static std::shared_ptr<SinkWorker> create(Args&&... args)
  {
    return std::make_shared<SinkWorker>(std::make_unique<T>(std::forward<Args>(args)...));
  }

  /// Stop accepting snapshots, wait for the callback in progress, join the
  /// worker thread and deliver everything still queued. Idempotent. Never call
  /// it from a callback.
  void stop();
  /// Resume after stop(): reopen admission and restart the worker thread.
  void start();
  /// Deliver every snapshot queued so far on the calling thread. Returns after
  /// a callback in progress on the worker has finished, so everything taken
  /// before the call has been delivered when it returns.
  void drain();

  DataSink& sink() { return *_sink; }
  const DataSink& sink() const { return *_sink; }
  /// Typed access to the owned sink, e.g. worker.as<MCAPSink>().restartRecording(...).
  /// Throws std::bad_cast if the sink is not a T.
  template <typename T>
  T& as()
  {
    return dynamic_cast<T&>(*_sink);
  }

  /// Number of onSnapshot() calls that threw, and the message of the last one.
  [[nodiscard]] uint64_t errors() const;
  [[nodiscard]] std::string lastError() const;

private:
  friend class LogChannel;
  std::unique_ptr<moodycamel::ProducerToken> makeProducerToken();
  bool tryPush(moodycamel::ProducerToken& token, SnapshotRef&& snapshot);
  /// Serialized with onSnapshot(); exceptions from onSchema() propagate.
  void addSchema(const Schema& schema);

  struct Pimpl;
  std::unique_ptr<Pimpl> _p;
  std::unique_ptr<DataSink> _sink;
};

//--------------------------------------------------

inline bool GetBit(const ActiveMask& mask, size_t index)
{
  const uint8_t& byte = mask[index >> 3];
  return 0 != (byte & uint8_t(1 << (index % 8)));
}

inline void SetBit(ActiveMask& mask, size_t index, bool value)
{
  if(!value)
  {
    mask[index >> 3] &= uint8_t(~(1 << (index % 8)));
  }
  else
  {
    mask[index >> 3] |= uint8_t(1 << (index % 8));
  }
}

}  // namespace DataTamer
