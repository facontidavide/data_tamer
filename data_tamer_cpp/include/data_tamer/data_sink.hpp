#pragma once

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
 * @brief Interface implemented by a sink. The callbacks are invoked only by
 * the SinkWorker that owns the sink and are serialized with each other, so a
 * sink needs no lock of its own for the state they touch:
 *
 * - onSchema() runs on the thread that attaches the channel to the sink
 *   (addDataSink or the first takeSnapshot), once per channel schema.
 * - onSnapshot() runs on the worker thread (or in drain()), in the order each
 *   channel took its snapshots. Throw to report a failure: the worker counts
 *   it and keeps the message (see SinkWorker).
 * - onStop() runs on the thread calling SinkWorker::stop() (or destroying the
 *   worker), after the last snapshot has been delivered: the place to close a
 *   file, publish a partial batch or write a pending dump. It runs once per
 *   stop: a second stop() without start() in between does not call it again.
 *   A throw is counted like one from onSnapshot(). The default does nothing.
 * - onStart() runs on the thread calling SinkWorker::start() after a stop()
 *   that called onStop(), before the worker accepts snapshots again: the place
 *   to reopen what onStop() closed. It is not called when the worker is
 *   constructed. A throw is counted like one from onSnapshot(). The default
 *   does nothing.
 *
 * Keep onSnapshot() short. Every queued or retained SnapshotRef holds a slot of
 * the channel's snapshot pool, which all sinks of that channel share
 * (SnapshotPool::kDefaultCapacity = 64 slots, see LogChannel::setPoolCapacity).
 * A sink that blocks in onSnapshot(), or retains its references, lets the
 * snapshots queued behind it pile up; once they occupy every slot,
 * takeSnapshot() and tryTakeSnapshot() return SnapshotResult::pool_exhausted
 * and the sample is lost for EVERY sink of the channel, not only the slow one.
 * At a 1 kHz control rate, 64 slots last 64 ms. A sink that needs to do slow
 * work (network, disk flushes, batching) should copy what it needs
 * (`Snapshot copy = *ref;`), hand the copy to its own thread and return, or
 * raise the pool capacity before prepare().
 *
 * A callback may use the channel's const queries (getSchema(), stats(), ...)
 * but must never call anything that changes it (registration, sinks,
 * prepare()): those wait for callbacks to finish and would deadlock.
 *
 * ABI: the virtual functions below are frozen for 2.x. The library calls them
 * through vtables compiled into user binaries, so adding, removing or
 * reordering one breaks every sink built against an earlier 2.x release.
 * New behaviour arrives as non-virtual functions of SinkWorker or as a
 * separate interface; DataSink keeps no data members.
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
 * @brief Owns a DataSink and the thread that delivers snapshots to it. It is
 * what LogChannel::addDataSink takes. The worker is stopped before the sink is
 * destroyed, so a sink never receives a callback while it is being torn down.
 *
 * Each channel attached to the worker publishes into a queue of its own, which
 * holds exactly as many snapshots as the channel's pool has slots
 * (LogChannel::setPoolCapacity) and is allocated with the pool, in prepare(), or
 * in addDataSink() on a prepared channel. A queued snapshot holds a pool slot,
 * so the queue cannot fill while the pool has a free slot: the pool is the only
 * bound, and there is no queue size to configure. Snapshots of one channel are
 * delivered in the order they were taken; the worker serves the channels in
 * turn, so snapshots of different channels interleave in no guaranteed order.
 * A channel detached from the worker (removeDataSink(), channel destroyed) has
 * its queued snapshots delivered before its queue is freed, and before what it
 * publishes after attaching to the worker again.
 *
 * An idle worker thread polls for about 20 us before it sleeps. A snapshot
 * pushed while it polls costs the snapshot thread an atomic increment and a
 * load; one pushed while it sleeps also wakes it (one futex wake on Linux).
 */
class SinkWorker
{
public:
  enum class Delivery : uint8_t
  {
    /// A worker thread delivers queued snapshots as they arrive (default).
    Threaded,
    /// No thread: the application delivers by calling drain() itself. The
    /// queue of a detached channel is freed by the next drain() (or stop(), or
    /// destruction), so a loop that detaches and attaches channels, or creates
    /// and destroys them, without draining keeps (pool slots + 1) x 24 bytes
    /// per cycle until then. A stopped Threaded worker behaves the same until
    /// start().
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

  /// Stop accepting snapshots, wait for the callback in progress, join the
  /// worker thread, deliver everything still queued, then call the sink's
  /// onStop() on this thread (MCAPSink closes its file, MCAPRingSink writes a
  /// pending dump, ROS2PublisherSink publishes its partial batch). Idempotent:
  /// calling it again before start() does not call onStop() a second time.
  /// The destructor calls it. Never call it from a callback. stop(), start()
  /// and the destructor must not run concurrently on the same worker: call
  /// them from one thread, or serialize them yourself.
  void stop();
  /// Resume after stop(): call the sink's onStart() on this thread (MCAPSink
  /// opens its next numbered file), restart the worker thread and accept
  /// snapshots again. Without a stop() since the last start, it does nothing.
  void start();
  /// Deliver every snapshot queued so far on the calling thread. Returns after
  /// a callback in progress on the worker has finished, so everything taken
  /// before the call has been delivered when it returns.
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

  /// Number of onSnapshot(), onStop() and onStart() calls that threw, and the
  /// message of the last one.
  [[nodiscard]] uint64_t errors() const;
  [[nodiscard]] std::string lastError() const;

private:
  friend class LogChannel;
  /// The queue of one channel on this worker; defined in data_sink.cpp.
  struct Attachment;
  /// Allocates a queue of `capacity` snapshots. Control path; may throw.
  Attachment* attach(size_t capacity);
  /// Ends an attachment: no push may follow. Its queued snapshots are still
  /// delivered; the queue is freed by the delivery pass that finds it empty.
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
