#pragma once

#include "data_tamer/channel.hpp"
#include "data_tamer/data_sink.hpp"

#include <memory>
#include <thread>
#include <utility>

namespace DataTamerTest
{

/// A SinkWorker owning a T, with direct access to the T. Converts to the
/// shared_ptr<SinkWorker> that LogChannel::addDataSink takes.
template <typename T>
struct Attached
{
  explicit Attached(std::shared_ptr<DataTamer::SinkWorker> worker_)
    : worker(std::move(worker_)), sink(&worker->template as<T>())
  {}

  template <typename... Args>
  explicit Attached(Args&&... args)
    : Attached(DataTamer::SinkWorker::create<T>(std::forward<Args>(args)...))
  {}

  T* operator->() const { return sink; }
  T& operator*() const { return *sink; }
  operator const std::shared_ptr<DataTamer::SinkWorker>&() const { return worker; }

  /// Deliver everything queued so far on this thread (see SinkWorker::drain).
  void drain() const { worker->drain(); }

  std::shared_ptr<DataTamer::SinkWorker> worker;
  T* sink;
};

/// Worker with an explicit delivery mode and queue capacity. Manual delivery
/// makes tests deterministic: snapshots are delivered only by drain().
template <typename T, typename... Args>
Attached<T> attach(DataTamer::SinkWorker::Delivery delivery,
                   size_t capacity = DataTamer::SinkWorker::kDefaultQueueCapacity,
                   Args&&... args)
{
  return Attached<T>(std::make_shared<DataTamer::SinkWorker>(
      std::make_unique<T>(std::forward<Args>(args)...), capacity, delivery));
}

template <typename T, typename... Args>
Attached<T> manual(Args&&... args)
{
  return Attached<T>(std::make_shared<DataTamer::SinkWorker>(
      std::make_unique<T>(std::forward<Args>(args)...),
      DataTamer::SinkWorker::kDefaultQueueCapacity,
      DataTamer::SinkWorker::Delivery::Manual));
}

/// True while the calling thread (or any other) holds the channel's write mutex:
/// a probe thread's tryTakeSnapshot() reports `blocked`. The channel must be
/// prepared with a sink attached (the probe takes a snapshot when not blocked).
inline bool writeMutexHeld(DataTamer::LogChannel& channel)
{
  bool held = false;
  std::thread([&] {
    held = channel.tryTakeSnapshot() == DataTamer::SnapshotResult::blocked;
  }).join();
  return held;
}

}  // namespace DataTamerTest
