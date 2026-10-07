#pragma once

#include "data_tamer/channel.hpp"
#include "data_tamer/data_sink.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
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

/// Worker with an explicit delivery mode. Manual delivery makes tests
/// deterministic: snapshots are delivered only by drain().
template <typename T, typename... Args>
Attached<T> attach(DataTamer::SinkWorker::Delivery delivery, Args&&... args)
{
  return Attached<T>(std::make_shared<DataTamer::SinkWorker>(
      std::make_unique<T>(std::forward<Args>(args)...), delivery));
}

template <typename T, typename... Args>
Attached<T> manual(Args&&... args)
{
  return attach<T>(DataTamer::SinkWorker::Delivery::Manual, std::forward<Args>(args)...);
}

/// A channel with one registered value, attached to `sink` (not prepared).
template <typename T>
std::shared_ptr<DataTamer::LogChannel>
channelWith(const std::shared_ptr<DataTamer::SinkWorker>& sink, T* value,
            const std::string& name = "test_channel")
{
  auto channel = DataTamer::LogChannel::create(name);
  channel->registerValue("value", value);
  channel->addDataSink(sink);
  return channel;
}

/// Snapshots a prepared channel accepts until its pool is exhausted. With
/// nothing delivered meanwhile, that is the pool capacity.
inline size_t acceptedUntilExhausted(DataTamer::LogChannel& channel)
{
  size_t accepted = 0;
  while(true)
  {
    const auto result = channel.tryTakeSnapshot();
    if(result != DataTamer::SnapshotResult::ok)
    {
      EXPECT_EQ(result, DataTamer::SnapshotResult::pool_exhausted);
      return accepted;
    }
    ++accepted;
  }
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
