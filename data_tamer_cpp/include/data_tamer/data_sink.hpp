#pragma once

#include "data_tamer/details/abi.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <typeinfo>
#include <vector>

namespace DataTamer
{

class LogChannel;
class SinkWorker;
class SnapshotPool;
struct PoolSlot;
struct Schema;

using ActiveMask = std::vector<uint8_t>;
using PayloadVector = std::vector<uint8_t>;

bool GetBit(const ActiveMask& mask, size_t index);
void SetBit(ActiveMask& mask, size_t index, bool val);

/// One captured sample of a channel. Fully owning: a copy is independent of the pool.
struct Snapshot
{
  /// Identifies the schema, and with it the channel (the hash covers the channel name).
  uint64_t schema_hash;
  /// Timestamp passed to takeSnapshot().
  std::chrono::nanoseconds timestamp;
  /// One bit per schema field (bit i % 8 of byte i / 8), set if the field is enabled.
  ActiveMask active_mask;
  /// Serialized values of the enabled fields, in schema order.
  PayloadVector payload;
};

/**
 * @brief Move-only handle to a pooled Snapshot. It holds one reference on the pool
 * slot, which is not reused until the last handle is released, even if the channel
 * is gone. clone() adds a reference to the same slot. `Snapshot copy = *ref;` makes
 * an independent copy.
 */
class SnapshotRef
{
public:
  SnapshotRef() = default;
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
  friend class SnapshotPool;  // SnapshotPool::adopt() is the only way to make one
  /// Takes ownership of one already-counted reference on `slot`.
  SnapshotRef(std::shared_ptr<SnapshotPool> pool, PoolSlot* slot);

  std::shared_ptr<SnapshotPool> pool_;
  PoolSlot* slot_ = nullptr;
};

/**
 * @brief Interface implemented by a sink. Only the owning SinkWorker calls the
 * callbacks, and it serializes them, so state they alone touch needs no lock.
 *
 * - onSchema() runs on the thread that calls LogChannel::startLogging() or
 *   addDataSink(), when a channel's schema reaches the sink (again after a re-attach).
 * - onSnapshot() runs on the worker thread (or in drain()), in the order each channel
 *   took its snapshots. A throw is caught and counted (SinkWorker::errors()).
 * - onStop() runs once per stop, on the thread that calls SinkWorker::stop() (or
 *   destroys the worker), after the last snapshot: close files, write what is pending.
 * - onStart() runs on the thread that calls SinkWorker::start() after a stop(), before
 *   snapshots are accepted again: reopen what onStop() closed. It is not called when
 *   the worker is constructed.
 *
 * onStop() and onStart() do nothing by default, and a throw from them is counted too.
 *
 * Keep onSnapshot() short. Every queued or retained SnapshotRef holds a slot of the
 * channel's snapshot pool, which all its sinks share (LogChannel::setPoolCapacity(),
 * 64 slots by default). A sink that blocks or retains references exhausts the pool, and
 * the samples are then lost for EVERY sink of the channel
 * (SnapshotResult::pool_exhausted). For slow work, copy the snapshot
 * (`Snapshot copy = *ref;`) and hand the copy to a thread of your own.
 *
 * A callback can call the channel's const queries (getSchema(), stats(), ...) but
 * never anything that changes it (registration, sinks, startLogging()): that deadlocks.
 *
 * ABI: the virtual functions are frozen for 2.x, because the library calls them through
 * vtables compiled into user binaries. New behaviour goes into non-virtual functions of
 * SinkWorker or into a separate interface, and DataSink keeps no data members.
 */
class DataSink
{
public:
  virtual ~DataSink() = default;

protected:
  friend class SinkWorker;
  virtual void onSchema(const Schema& schema) = 0;
  virtual void onSnapshot(const SnapshotRef& snapshot) = 0;
  virtual void onStop() {}
  virtual void onStart() {}
};

/**
 * @brief Owns a DataSink and the thread that delivers snapshots to it. It is what
 * LogChannel::addDataSink() takes. The worker is stopped before the sink is destroyed,
 * so a callback never runs on a sink being torn down.
 *
 * Each attached channel has its own queue, as large as the channel's pool. A queued
 * snapshot holds a pool slot, so the pool is the only bound and there is no queue size
 * to configure. Snapshots of one channel are delivered in the order they were taken, also
 * across removeDataSink() and addDataSink(). Snapshots of different channels interleave
 * in no guaranteed order. The queued snapshots of a detached channel are still delivered.
 */
class SinkWorker
{
public:
  enum class Delivery : uint8_t
  {
    /// A worker thread delivers queued snapshots as they arrive (default).
    Threaded,
    /// No thread: the application calls drain(). The queue of a detached channel is
    /// freed only by the next drain(), stop() or destruction, so attaching and
    /// detaching channels in a loop without draining accumulates memory.
    Manual
  };

  explicit SinkWorker(std::unique_ptr<DataSink> sink,
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

  /// Stop accepting snapshots, wait for the callback in progress, deliver everything
  /// still queued, then call the sink's onStop() on this thread (MCAPSink closes its
  /// file, MCAPRingSink writes a pending dump, ROS2PublisherSink publishes its partial
  /// batch). A second stop() before start() does nothing. The destructor calls it, or,
  /// when it runs inside a callback of this worker's sink (the last reference dropped
  /// there), leaves the stop to a thread of its own that runs once the callback has
  /// returned. Throws std::logic_error, changing nothing, when called from a callback
  /// of this worker's sink. Calls to stop() and start() from several threads are
  /// serialized.
  void stop();
  /// Resume after stop(): call the sink's onStart() on this thread (MCAPSink opens its
  /// next numbered file), restart the worker thread and accept snapshots again. Does
  /// nothing if the worker is not stopped. Throws std::logic_error from a callback of
  /// this worker's sink.
  void start();
  /// Deliver, on the calling thread, every snapshot taken before the call. Waits for a
  /// callback in progress on the worker. Throws std::logic_error from a callback of
  /// this worker's sink.
  void drain();

  DataSink& sink();
  const DataSink& sink() const;
  /// Typed access to the owned sink, e.g. worker.as<MCAPSink>().restartRecording(...).
  /// Throws std::bad_cast if the sink is not a T.
  template <typename T>
  T& as()
  {
    return dynamic_cast<T&>(sink());
  }

  /// Snapshots handed to the sink so far: onSnapshot() calls that returned.
  [[nodiscard]] uint64_t delivered() const;

  /// Number of onSnapshot(), onStop() and onStart() calls that threw, and the
  /// message of the last one.
  [[nodiscard]] uint64_t errors() const;
  [[nodiscard]] std::string lastError() const;

  /// The most snapshots found waiting in one queue, over all channels attached to this
  /// worker (per worker, not per channel). It is a lower bound, and zero before the
  /// first delivery. A value near the largest pool capacity among those channels means
  /// the sink nearly exhausted a pool.
  [[nodiscard]] uint64_t queueHighWater() const;

  /// Everything the worker counts, cumulative over the worker's life (stop() and
  /// start() do not reset it).
  struct Stats
  {
    uint64_t delivered = 0;
    uint64_t errors = 0;
    /// Per worker, across all its channels; see queueHighWater().
    uint64_t queue_high_water = 0;
    /// Message of the last throw, never older than `errors`.
    std::string last_error;
  };

  /// The fields are read one by one, not as one instant. Not real-time safe:
  /// lastError() locks a mutex and allocates. On a real-time thread call delivered(),
  /// errors() and queueHighWater(), which are atomic loads. Inline on purpose (built
  /// from the getters above), so Stats can gain fields without an ABI break.
  [[nodiscard]] DATA_TAMER_INLINE_LOCAL Stats stats() const
  {
    return { delivered(), errors(), queueHighWater(), lastError() };
  }

private:
  friend class LogChannel;
  /// The queue of one channel on this worker; defined in data_sink.cpp.
  struct Attachment;
  /// Allocates a queue of `capacity` snapshots. Control path; may throw.
  Attachment* attach(size_t capacity);
  /// No push may follow. Queued snapshots are still delivered, then the queue is freed.
  void detach(Attachment* attachment) noexcept;
  /// Real-time path: no allocation, no lock. False when the worker is stopped.
  bool tryPush(Attachment& attachment, SnapshotRef&& snapshot);
  /// Serialized with onSnapshot(); exceptions from onSchema() propagate.
  void addSchema(const Schema& schema);

  struct Pimpl;  // holds the sink too: the layout is one pointer
  std::unique_ptr<Pimpl> _p;
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
