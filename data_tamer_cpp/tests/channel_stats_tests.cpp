// LogChannel::Stats (attempts, accepted, dropped_by_sink) and SinkWorker::Stats.
#include "data_tamer/channel.hpp"
#include "data_tamer/data_sink.hpp"
#include "alloc_counter.hpp"
#include "test_sinks.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace DataTamer;
using DataTamerTest::attach;
using DataTamerTest::Attached;
using DataTamerTest::manual;
using Delivery = SinkWorker::Delivery;

namespace
{
class CountingSink : public DataSink
{
public:
  void onSchema(const Schema&) override {}
  void onSnapshot(const SnapshotRef&) override
  {
    ++snapshots;
    if(throw_every > 0 && snapshots % throw_every == 0)
    {
      throw std::runtime_error("sink failed " + std::to_string(snapshots));
    }
  }
  int snapshots = 0;
  int throw_every = 0;  // 0: never
};

uint64_t droppedFor(const LogChannel::Stats& stats, const Attached<CountingSink>& sink)
{
  for(const auto& entry : stats.dropped_by_sink)
  {
    if(entry.sink == sink.worker.get())
    {
      return entry.dropped;
    }
  }
  ADD_FAILURE() << "sink not in dropped_by_sink";
  return 0;
}
}  // namespace

TEST(ChannelStats, CountersMatchEveryResultOfAScriptedSequence)
{
  auto channel = LogChannel::create("stats");
  auto value = channel->createLoggedValue<std::vector<double>>("value", { 1.0 });
  channel->setPoolCapacity(3);
  channel->setPayloadCapacity(256);
  auto a = manual<CountingSink>();
  auto b = manual<CountingSink>();

  std::map<SnapshotResult, uint64_t> seen;
  uint64_t calls = 0;
  const auto expect = [&](SnapshotResult got, SnapshotResult want) {
    EXPECT_EQ(got, want);
    ++seen[got];
    ++calls;
    const auto stats = channel->stats();
    EXPECT_EQ(stats.attempts, calls);
    EXPECT_EQ(stats.accepted, seen[SnapshotResult::ok] + seen[SnapshotResult::partial]);
  };

  expect(channel->takeSnapshot(), SnapshotResult::no_sinks);
  channel->addDataSink(a);
  expect(channel->tryTakeSnapshot(), SnapshotResult::not_prepared);
  expect(channel->takeSnapshot(), SnapshotResult::ok);  // prepares

  channel->addDataSink(b);
  b.worker->stop();
  expect(channel->takeSnapshot(), SnapshotResult::partial);
  a.worker->stop();
  expect(channel->takeSnapshot(), SnapshotResult::rejected);
  a.worker->start();
  b.worker->start();
  // stop() delivered what A held. The three slots now go to three snapshots that
  // B and A both hold.
  for(int i = 0; i < 3; ++i)
  {
    expect(channel->tryTakeSnapshot(), SnapshotResult::ok);
  }
  expect(channel->tryTakeSnapshot(), SnapshotResult::pool_exhausted);
  expect(channel->takeSnapshot(), SnapshotResult::pool_exhausted);
  a.drain();
  b.drain();

  value->set(std::vector<double>(1024, 3.0));
  expect(channel->tryTakeSnapshot(), SnapshotResult::oversize);
  value->set(std::vector<double>{ 1.0 });

  SnapshotResult probe = SnapshotResult::ok;
  {
    // stats() takes the control mutex, which must not be taken inside a
    // scopedWrite(): read the counters after the scope.
    const auto transaction = channel->scopedWrite();
    std::thread([&] { probe = channel->tryTakeSnapshot(); }).join();
  }
  expect(probe, SnapshotResult::blocked);
  expect(channel->tryTakeSnapshot(), SnapshotResult::ok);

  // Every result of the script was exercised, and the counters add up.
  for(auto result : { SnapshotResult::ok, SnapshotResult::partial,
                      SnapshotResult::rejected, SnapshotResult::no_sinks,
                      SnapshotResult::not_prepared, SnapshotResult::pool_exhausted,
                      SnapshotResult::oversize, SnapshotResult::blocked })
  {
    EXPECT_GE(seen[result], 1u) << "result " << int(result) << " never happened";
  }
  const auto stats = channel->stats();
  EXPECT_EQ(stats.attempts, 13u);
  EXPECT_EQ(stats.accepted, 6u);  // 5 ok + 1 partial
  EXPECT_EQ(stats.pool_exhausted, 2u);
  EXPECT_EQ(stats.dropped_oversize, 1u);
  EXPECT_EQ(stats.write_lock_contended, 1u);
  EXPECT_EQ(channel->snapshotAttempts(), stats.attempts);
  EXPECT_EQ(channel->snapshotsAccepted(), stats.accepted);
}

TEST(ChannelStats, ExistingCountersKeepTheirMeaning)
{
  auto channel = LogChannel::create("stats");
  auto value = channel->createLoggedValue<std::vector<double>>("value", { 1.0 });
  channel->setPoolCapacity(2);
  channel->setPayloadCapacity(256);
  auto sink = manual<CountingSink>();
  channel->addDataSink(sink);
  ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
  value->set(std::vector<double>(1024, 3.0));
  ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);  // grows the second slot
  ASSERT_EQ(channel->tryTakeSnapshot(), SnapshotResult::pool_exhausted);
  sink.drain();
  ASSERT_EQ(channel->tryTakeSnapshot(), SnapshotResult::oversize);
  const auto stats = channel->stats();
  EXPECT_EQ(stats.pool_exhausted, channel->poolExhausted());
  EXPECT_EQ(stats.pool_exhausted, 1u);
  EXPECT_EQ(stats.payload_reallocations, channel->payloadReallocations());
  EXPECT_EQ(stats.payload_reallocations, 1u);
  EXPECT_EQ(stats.dropped_oversize, channel->droppedOversize());
  EXPECT_EQ(stats.dropped_oversize, 1u);
  EXPECT_EQ(stats.write_lock_contended, channel->writeLockContended());
  EXPECT_EQ(stats.write_lock_wait_max_ns, channel->writeLockWaitMaxNs());
  EXPECT_EQ(stats.attempts, 4u);
  EXPECT_EQ(stats.accepted, 2u);
}

TEST(ChannelStats, DroppedIsCountedPerSink)
{
  auto channel = LogChannel::create("stats");
  uint64_t value = 1;
  channel->registerValue("value", &value);
  auto a = manual<CountingSink>();
  auto b = manual<CountingSink>();
  auto c = manual<CountingSink>();
  channel->addDataSink(a);
  channel->addDataSink(b);
  channel->addDataSink(c);
  EXPECT_EQ(channel->stats().dropped_by_sink.size(), 3u);

  b.worker->stop();
  for(int i = 0; i < 3; ++i)
  {
    ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::partial);
    a.drain();
    c.drain();
  }
  c.worker->stop();
  ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::partial);
  a.drain();

  auto stats = channel->stats();
  ASSERT_EQ(stats.dropped_by_sink.size(), 3u);
  EXPECT_EQ(droppedFor(stats, a), 0u);
  EXPECT_EQ(droppedFor(stats, b), 4u);
  EXPECT_EQ(droppedFor(stats, c), 1u);
  EXPECT_EQ(stats.accepted, 4u);
  EXPECT_EQ(stats.attempts, 4u);

  // The deprecated per-sink getter agrees.
  EXPECT_EQ(channel->droppedSnapshots(a), 0u);
  EXPECT_EQ(channel->droppedSnapshots(b), 4u);
  EXPECT_EQ(channel->droppedSnapshots(c), 1u);

  // sinkDropped() is the building block: one call returns the whole table, and
  // the count of attached sinks even when the arrays are too small for it.
  const SinkWorker* workers[LogChannel::kMaxSinks];
  uint64_t drops[LogChannel::kMaxSinks];
  EXPECT_EQ(channel->sinkDropped(workers, drops, LogChannel::kMaxSinks), 3u);
  EXPECT_EQ(channel->sinkDropped(workers, drops, 2), 3u);
  EXPECT_EQ(channel->sinkDropped(workers, drops, 0), 3u);

  // A removed sink leaves the array and its count goes with it; the others stay.
  channel->removeDataSink(b);
  stats = channel->stats();
  ASSERT_EQ(stats.dropped_by_sink.size(), 2u);
  EXPECT_EQ(droppedFor(stats, c), 1u);
  EXPECT_EQ(channel->droppedSnapshots(b), 0u);
}

TEST(ChannelStats, ChannelWithoutSinksHasAnEmptyDroppedArray)
{
  auto channel = LogChannel::create("stats");
  const auto stats = channel->stats();
  EXPECT_EQ(stats.attempts, 0u);
  EXPECT_EQ(stats.accepted, 0u);
  EXPECT_TRUE(stats.dropped_by_sink.empty());
}

TEST(ChannelStats, TheSnapshotPathStillAllocatesNothing)
{
  auto channel = LogChannel::create("stats");
  double value = 1.0;
  channel->registerValue("value", &value);
  channel->setPoolCapacity(4);
  auto sink = manual<CountingSink>();
  channel->addDataSink(sink);
  channel->prepare();
  ASSERT_EQ(channel->tryTakeSnapshot(), SnapshotResult::ok);
  sink.drain();
  {
    DataTamerTest::AllocCounter::Scope scope;
    for(int i = 0; i < 4; ++i)
    {
      ASSERT_EQ(channel->tryTakeSnapshot(), SnapshotResult::ok);
    }
    EXPECT_EQ(channel->tryTakeSnapshot(), SnapshotResult::pool_exhausted);
    EXPECT_EQ(scope.allocations(), 0u);
  }
  EXPECT_EQ(channel->snapshotAttempts(), 6u);
  EXPECT_EQ(channel->snapshotsAccepted(), 5u);
}

TEST(ChannelStats, CountersAreExactWhileAnotherThreadReadsThem)
{
  auto channel = LogChannel::create("stats");
  double value = 1.0;
  channel->registerValue("value", &value);
  channel->setPoolCapacity(8);
  auto sink = attach<CountingSink>(Delivery::Threaded);
  channel->addDataSink(sink);
  channel->prepare();

  std::atomic<bool> done{ false };
  std::thread reader([&] {
    uint64_t previous = 0;
    while(!done)
    {
      const auto stats = channel->stats();
      EXPECT_LE(stats.accepted, stats.attempts) << "within one read";
      EXPECT_GE(stats.attempts, previous);
      previous = stats.attempts;
    }
  });
  constexpr uint64_t kCalls = 20000;
  uint64_t accepted = 0;
  for(uint64_t i = 0; i < kCalls; ++i)
  {
    accepted += channel->tryTakeSnapshot() == SnapshotResult::ok;
  }
  done = true;
  reader.join();
  sink.drain();
  const auto stats = channel->stats();
  EXPECT_EQ(stats.attempts, kCalls);
  EXPECT_EQ(stats.accepted, accepted);
  EXPECT_EQ(sink.worker->stats().delivered, accepted);
}

TEST(SinkWorkerStats, DeliveredCountsSnapshotsHandedToTheSink)
{
  auto channel = LogChannel::create("stats");
  uint64_t value = 1;
  channel->registerValue("value", &value);
  channel->setPoolCapacity(10);
  auto sink = manual<CountingSink>();
  channel->addDataSink(sink);

  auto stats = sink.worker->stats();
  EXPECT_EQ(stats.delivered, 0u);
  EXPECT_EQ(stats.errors, 0u);
  EXPECT_EQ(stats.queue_high_water, 0u);
  EXPECT_TRUE(stats.last_error.empty());

  for(int i = 0; i < 7; ++i)
  {
    ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
  }
  EXPECT_EQ(sink.worker->stats().delivered, 0u) << "queued, not yet delivered";
  sink.drain();
  stats = sink.worker->stats();
  EXPECT_EQ(stats.delivered, 7u);
  EXPECT_EQ(stats.delivered, sink.worker->delivered());
  EXPECT_EQ(stats.errors, 0u);
  EXPECT_EQ(stats.queue_high_water, 7u);

  // The high-water mark only grows, and the counters survive stop() and start().
  ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
  sink.drain();
  sink.worker->stop();
  sink.worker->start();
  stats = sink.worker->stats();
  EXPECT_EQ(stats.delivered, 8u);
  EXPECT_EQ(stats.queue_high_water, 7u);
}

TEST(SinkWorkerStats, ThrowingSinkIsCountedAndNotDelivered)
{
  auto channel = LogChannel::create("stats");
  uint64_t value = 1;
  channel->registerValue("value", &value);
  auto sink = manual<CountingSink>();
  sink->throw_every = 3;
  channel->addDataSink(sink);
  for(int i = 0; i < 9; ++i)
  {
    ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
    sink.drain();
  }
  const auto stats = sink.worker->stats();
  EXPECT_EQ(sink->snapshots, 9);
  EXPECT_EQ(stats.errors, 3u);
  EXPECT_EQ(stats.delivered, 6u);
  EXPECT_EQ(stats.last_error, "sink failed 9");
  EXPECT_EQ(stats.errors, sink.worker->errors());
  EXPECT_EQ(stats.last_error, sink.worker->lastError());
}

TEST(SinkWorkerStats, HighWaterIsTheDeepestChannelQueue)
{
  auto sink = manual<CountingSink>();
  uint64_t value = 1;
  auto shallow = LogChannel::create("shallow");
  auto deep = LogChannel::create("deep");
  shallow->registerValue("value", &value);
  deep->registerValue("value", &value);
  deep->setPoolCapacity(16);
  shallow->addDataSink(sink);
  deep->addDataSink(sink);
  for(int i = 0; i < 3; ++i)
  {
    ASSERT_EQ(shallow->takeSnapshot(), SnapshotResult::ok);
  }
  for(int i = 0; i < 12; ++i)
  {
    ASSERT_EQ(deep->takeSnapshot(), SnapshotResult::ok);
  }
  sink.drain();
  EXPECT_EQ(sink.worker->stats().delivered, 15u);
  EXPECT_EQ(sink.worker->queueHighWater(), 12u);
}

TEST(SinkWorkerStats, ThreadedWorkerCountsEverythingItDelivers)
{
  auto channel = LogChannel::create("stats");
  uint64_t value = 1;
  channel->registerValue("value", &value);
  channel->setPoolCapacity(64);
  auto sink = attach<CountingSink>(Delivery::Threaded);
  channel->addDataSink(sink);
  channel->prepare();
  uint64_t accepted = 0;
  for(int i = 0; i < 5000; ++i)
  {
    accepted += channel->tryTakeSnapshot() == SnapshotResult::ok;
  }
  sink.drain();
  const auto stats = sink.worker->stats();
  EXPECT_EQ(stats.delivered, accepted);
  EXPECT_GT(accepted, 0u);
  EXPECT_GE(stats.queue_high_water, 1u);
  EXPECT_LE(stats.queue_high_water, 64u);
}

TEST(ChannelStats, RefusedLastPushWhileAnotherSinkAcceptsIsPartial)
{
  auto channel = LogChannel::create("stats");
  uint64_t value = 1;
  channel->registerValue("value", &value);
  channel->setPoolCapacity(1);
  // A channel fills its table from the highest slot down, so the first sink
  // attached is the last one published to: the snapshot is moved into its push.
  auto last = manual<CountingSink>();
  auto first = manual<CountingSink>();
  channel->addDataSink(last);
  channel->addDataSink(first);
  channel->prepare();
  last.worker->stop();

  for(uint64_t round = 1; round <= 3; ++round)
  {
    // The single pool slot comes back each round: the refused push released its
    // reference, and the accepting sink gives its own up when it drains.
    ASSERT_EQ(channel->tryTakeSnapshot(), SnapshotResult::partial) << "round " << round;
    first.drain();
    const auto stats = channel->stats();
    EXPECT_EQ(stats.attempts, round);
    EXPECT_EQ(stats.accepted, round);
    EXPECT_EQ(droppedFor(stats, last), round);
    EXPECT_EQ(droppedFor(stats, first), 0u);
    EXPECT_EQ(stats.pool_exhausted, 0u);
  }
  EXPECT_EQ(first.worker->stats().delivered, 3u);
}

namespace
{
class SchemaRefusingSink : public DataSink
{
public:
  void onSchema(const Schema&) override { throw std::runtime_error("no schema"); }
  void onSnapshot(const SnapshotRef&) override {}
};
}  // namespace

TEST(ChannelStats, ACallThatThrowsCountsAsAnAttemptOnly)
{
  auto channel = LogChannel::create("stats");
  uint64_t value = 1;
  channel->registerValue("value", &value);
  auto refusing = manual<SchemaRefusingSink>();
  channel->addDataSink(refusing);
  EXPECT_THROW((void)channel->takeSnapshot(), std::runtime_error);
  auto stats = channel->stats();
  EXPECT_EQ(stats.attempts, 1u);
  EXPECT_EQ(stats.accepted, 0u);
  EXPECT_THROW((void)channel->takeSnapshot(), std::runtime_error);
  stats = channel->stats();
  EXPECT_EQ(stats.attempts, 2u);
  EXPECT_EQ(stats.accepted, 0u);
}

TEST(SinkWorkerStats, HighWaterSeesAQueueThatWrapsAroundItsRing)
{
  auto channel = LogChannel::create("stats");
  uint64_t value = 1;
  channel->registerValue("value", &value);
  channel->setPoolCapacity(4);  // a ring of five entries
  auto sink = manual<CountingSink>();
  channel->addDataSink(sink);
  for(int i = 0; i < 3; ++i)
  {
    ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
  }
  sink.drain();
  EXPECT_EQ(sink.worker->queueHighWater(), 3u);
  // The ring is empty at position 3: four more entries wrap past the end.
  for(int i = 0; i < 4; ++i)
  {
    ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
  }
  sink.drain();
  EXPECT_EQ(sink.worker->queueHighWater(), 4u);
  EXPECT_EQ(sink.worker->delivered(), 7u);
}
