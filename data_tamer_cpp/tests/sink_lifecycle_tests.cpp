// DataSink::onStop() and onStart(), ChannelsRegistry::stopAll(),
// addDefaultSink() on existing channels and ChannelsRegistry::setChannelDefaults().
#include "data_tamer/channel.hpp"
#include "data_tamer/data_tamer.hpp"
#include "data_tamer/sinks/mcap_sink.hpp"
#include "mcap_test_utils.hpp"
#include "test_sinks.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace DataTamer;
using DataTamerTest::acceptedUntilExhausted;
using DataTamerTest::attach;
using DataTamerTest::channelWith;
using DataTamerTest::countMessages;
using DataTamerTest::manual;
using Delivery = SinkWorker::Delivery;
using std::chrono::milliseconds;
using std::chrono::nanoseconds;

namespace
{
// What a LifecycleSink saw. Shared with the test so that it survives the sink.
struct Journal
{
  mutable std::mutex mutex;
  std::unordered_map<std::string, int> schemas;  // channel name -> onSchema() calls
  int snapshots = 0;
  int stops = 0;
  int starts = 0;
  int snapshots_at_stop = -1;
  std::thread::id stop_thread;
  bool snapshot_in_progress_at_stop = false;
  bool throw_on_stop = false;

  // Optional gate: the first onSnapshot() announces itself, then waits for release.
  bool gate_first_snapshot = false;
  bool entered = false;
  bool released = false;
  bool in_snapshot = false;
  std::condition_variable cv;

  int stopCount() const
  {
    std::lock_guard lock(mutex);
    return stops;
  }
  int startCount() const
  {
    std::lock_guard lock(mutex);
    return starts;
  }
  int snapshotCount() const
  {
    std::lock_guard lock(mutex);
    return snapshots;
  }
  int schemaCount(const std::string& channel) const
  {
    std::lock_guard lock(mutex);
    const auto it = schemas.find(channel);
    return it == schemas.end() ? 0 : it->second;
  }
};

class LifecycleSink : public DataSink
{
public:
  explicit LifecycleSink(std::shared_ptr<Journal> journal) : journal_(std::move(journal))
  {}

protected:
  void onSchema(const Schema& schema) override
  {
    std::lock_guard lock(journal_->mutex);
    ++journal_->schemas[schema.channel_name];
  }

  void onSnapshot(const SnapshotRef&) override
  {
    std::unique_lock lock(journal_->mutex);
    journal_->in_snapshot = true;
    if(journal_->gate_first_snapshot && !journal_->entered)
    {
      journal_->entered = true;
      journal_->cv.notify_all();
      journal_->cv.wait(lock, [this] { return journal_->released; });
    }
    ++journal_->snapshots;
    journal_->in_snapshot = false;
  }

  void onStop() override
  {
    std::lock_guard lock(journal_->mutex);
    ++journal_->stops;
    journal_->snapshots_at_stop = journal_->snapshots;
    journal_->stop_thread = std::this_thread::get_id();
    journal_->snapshot_in_progress_at_stop = journal_->in_snapshot;
    if(journal_->throw_on_stop)
    {
      throw std::runtime_error("stop failed");
    }
  }

  void onStart() override
  {
    std::lock_guard lock(journal_->mutex);
    ++journal_->starts;
  }

private:
  std::shared_ptr<Journal> journal_;
};

}  // namespace

//------------------------------------------------------------------------------
// DataSink::onStop()

TEST(SinkLifecycle, StopDeliversEverythingThenCallsOnStopOnceOnTheStoppingThread)
{
  for(const auto delivery : { Delivery::Threaded, Delivery::Manual })
  {
    auto journal = std::make_shared<Journal>();
    auto worker = attach<LifecycleSink>(delivery, journal).worker;
    double value = 1.0;
    auto channel = channelWith(worker, &value, "lifecycle");
    for(int i = 0; i < 10; ++i)
    {
      ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
    }
    worker->stop();
    {
      std::lock_guard lock(journal->mutex);
      EXPECT_EQ(journal->stops, 1);
      EXPECT_EQ(journal->snapshots_at_stop, 10) << "onStop() after the last delivery";
      EXPECT_EQ(journal->stop_thread, std::this_thread::get_id());
    }
    EXPECT_EQ(channel->takeSnapshot(), SnapshotResult::rejected);

    worker->stop();  // idempotent
    EXPECT_EQ(journal->stopCount(), 1);
    EXPECT_EQ(journal->startCount(), 0) << "not called on construction";

    // Each start()/stop() cycle starts and finishes the sink again.
    worker->start();
    worker->start();  // running already: no second onStart()
    EXPECT_EQ(journal->startCount(), 1);
    for(int i = 0; i < 5; ++i)
    {
      ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
    }
    worker->stop();
    {
      std::lock_guard lock(journal->mutex);
      EXPECT_EQ(journal->stops, 2);
      EXPECT_EQ(journal->snapshots_at_stop, 15);
    }
    channel.reset();
    worker.reset();  // stopped already: the destructor does not call onStop()
    EXPECT_EQ(journal->stopCount(), 2);
    EXPECT_EQ(journal->snapshotCount(), 15);
  }
}

TEST(SinkLifecycle, DestroyingARunningWorkerFinishesTheSink)
{
  for(const auto delivery : { Delivery::Threaded, Delivery::Manual })
  {
    auto journal = std::make_shared<Journal>();
    {
      auto worker = attach<LifecycleSink>(delivery, journal).worker;
      double value = 1.0;
      auto channel = channelWith(worker, &value, "lifecycle_destroy");
      for(int i = 0; i < 3; ++i)
      {
        ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
      }
    }  // channel first, then the last reference to the worker
    std::lock_guard lock(journal->mutex);
    EXPECT_EQ(journal->stops, 1);
    EXPECT_EQ(journal->snapshots_at_stop, 3);
  }
}

TEST(SinkLifecycle, OnStopThrowIsCountedAndDoesNotEscape)
{
  auto journal = std::make_shared<Journal>();
  journal->throw_on_stop = true;
  auto worker = manual<LifecycleSink>(journal).worker;
  EXPECT_NO_THROW(worker->stop());
  EXPECT_EQ(worker->errors(), 1u);
  EXPECT_EQ(worker->lastError(), "stop failed");
  worker->start();
  EXPECT_NO_THROW(worker->stop());
  EXPECT_EQ(worker->errors(), 2u);
  EXPECT_EQ(journal->stopCount(), 2);
  EXPECT_NO_THROW(worker.reset());
  EXPECT_EQ(journal->stopCount(), 2);
}

// onStop() is serialized with onSnapshot(): a stop() racing a callback in
// progress closes admission at once, waits for the callback, delivers every
// snapshot accepted until the close, then finishes. start() resumes.
TEST(SinkLifecycle, OnStopWaitsForTheCallbackInProgressAndTheQueue)
{
  auto journal = std::make_shared<Journal>();
  journal->gate_first_snapshot = true;
  auto worker = attach<LifecycleSink>(Delivery::Threaded, journal).worker;
  double value = 1.0;
  auto channel = channelWith(worker, &value, "lifecycle_race");
  ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
  {
    std::unique_lock lock(journal->mutex);
    ASSERT_TRUE(journal->cv.wait_for(lock, std::chrono::seconds(5),
                                     [&] { return journal->entered; }));
  }
  int accepted = 1;
  ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);  // queued behind the gate
  ++accepted;
  std::thread stopper([&] { worker->stop(); });
  // Take snapshots until one is refused: the pool runs out behind the gated
  // callback, or stop() has closed admission. Either way, every accepted one
  // must reach onSnapshot() before onStop().
  while(channel->takeSnapshot() == SnapshotResult::ok)
  {
    ++accepted;
  }
  EXPECT_EQ(journal->stopCount(), 0) << "onStop() ran during onSnapshot()";
  {
    std::lock_guard lock(journal->mutex);
    journal->released = true;
  }
  journal->cv.notify_all();
  const auto stopper_id = stopper.get_id();
  stopper.join();
  {
    std::lock_guard lock(journal->mutex);
    EXPECT_EQ(journal->stops, 1);
    EXPECT_FALSE(journal->snapshot_in_progress_at_stop);
    EXPECT_EQ(journal->snapshots_at_stop, accepted);
    EXPECT_EQ(journal->stop_thread, stopper_id);
  }
  worker->start();
  EXPECT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
  worker->drain();
  EXPECT_EQ(journal->snapshotCount(), accepted + 1);
  EXPECT_EQ(journal->startCount(), 1);
  worker->stop();
}

// SinkWorker::stop() alone yields a complete, finalized MCAP file (summary and
// footer written): no stopRecording() needed. restartRecording() picks the
// next file itself; start() then leaves it alone.
TEST(SinkLifecycle, McapStopFinalizesTheFileAndRestartRecordsAgain)
{
  const auto path = DataTamerTest::tempPath("lifecycle_mcap");
  const auto second_path = DataTamerTest::tempPath("lifecycle_mcap_second");
  auto worker = MCAPSink::create(path);
  double value = 1.0;
  auto channel = channelWith(worker, &value, "lifecycle_mcap");
  for(int i = 0; i < 7; ++i)
  {
    ASSERT_EQ(channel->takeSnapshot(nanoseconds(i + 1)), SnapshotResult::ok);
  }
  worker->stop();
  EXPECT_EQ(worker->errors(), 0u);
  EXPECT_EQ(countMessages(path, true), 7u);

  worker->as<MCAPSink>().restartRecording(second_path);
  worker->start();
  for(int i = 0; i < 4; ++i)
  {
    ASSERT_EQ(channel->takeSnapshot(nanoseconds(100 + i)), SnapshotResult::ok);
  }
  channel.reset();
  worker.reset();  // the destructor stops, which finalizes the second file
  EXPECT_EQ(countMessages(second_path, true), 4u);
  EXPECT_EQ(countMessages(path, true), 7u) << "the first file is left alone";
  EXPECT_FALSE(std::filesystem::exists(details::NumberedPath(second_path, 1)));
  std::filesystem::remove(path);
  std::filesystem::remove(second_path);
}

// stop() then start() without restartRecording(): onStart() records into the
// next numbered file, so no snapshot accepted after start() is dropped.
TEST(SinkLifecycle, McapStopThenStartRecordsIntoTheNextNumberedFile)
{
  const auto path = DataTamerTest::tempPath("lifecycle_mcap_cycle");
  const auto next_path = details::NumberedPath(path, 1);
  auto worker = MCAPSink::create(path);
  double value = 1.0;
  auto channel = channelWith(worker, &value, "lifecycle_mcap_cycle");
  for(int i = 0; i < 3; ++i)
  {
    ASSERT_EQ(channel->takeSnapshot(nanoseconds(i + 1)), SnapshotResult::ok);
  }
  worker->stop();
  worker->start();
  for(int i = 0; i < 5; ++i)
  {
    ASSERT_EQ(channel->takeSnapshot(nanoseconds(100 + i)), SnapshotResult::ok);
  }
  worker->stop();
  EXPECT_EQ(worker->errors(), 0u);
  EXPECT_EQ(countMessages(path, true), 3u);
  EXPECT_EQ(countMessages(next_path, true), 5u);
  std::filesystem::remove(path);
  std::filesystem::remove(next_path);
}

//------------------------------------------------------------------------------
// ChannelsRegistry

TEST(ChannelsRegistry, AddDefaultSinkReachesExistingChannels)
{
  ChannelsRegistry registry;
  double value = 1.0;
  auto other_journal = std::make_shared<Journal>();
  auto other = manual<LifecycleSink>(other_journal).worker;

  auto open_channel = registry.getChannel("open");
  open_channel->registerValue("value", &value);
  auto prepared_channel = registry.getChannel("prepared");
  prepared_channel->registerValue("value", &value);
  prepared_channel->addDataSink(other);
  prepared_channel->prepare();

  auto journal = std::make_shared<Journal>();
  auto sink = manual<LifecycleSink>(journal).worker;
  registry.addDefaultSink(sink);
  // The prepared channel announced its schema at once; the open one will at prepare().
  EXPECT_EQ(journal->schemaCount("prepared"), 1);
  EXPECT_EQ(journal->schemaCount("open"), 0);
  EXPECT_EQ(open_channel->getNumberOfSinks(), 1u);
  EXPECT_EQ(prepared_channel->getNumberOfSinks(), 2u);

  auto later = registry.getChannel("later");
  later->registerValue("value", &value);
  EXPECT_EQ(later->getNumberOfSinks(), 1u);

  ASSERT_EQ(open_channel->takeSnapshot(), SnapshotResult::ok);
  ASSERT_EQ(prepared_channel->tryTakeSnapshot(), SnapshotResult::ok);
  ASSERT_EQ(later->takeSnapshot(), SnapshotResult::ok);
  sink->drain();
  EXPECT_EQ(journal->schemaCount("open"), 1);
  EXPECT_EQ(journal->schemaCount("prepared"), 1) << "announced once";
  EXPECT_EQ(journal->schemaCount("later"), 1);
  EXPECT_EQ(journal->snapshotCount(), 3);

  // Adding it again changes nothing.
  registry.addDefaultSink(sink);
  EXPECT_EQ(prepared_channel->getNumberOfSinks(), 2u);
  EXPECT_EQ(journal->schemaCount("prepared"), 1);
}

TEST(ChannelsRegistry, AddDefaultSinkIsUndoneWhenAChannelRefusesIt)
{
  ChannelsRegistry registry;
  auto shared_journal = std::make_shared<Journal>();
  auto shared = manual<LifecycleSink>(shared_journal).worker;

  auto full = registry.getChannel("full");
  std::vector<std::shared_ptr<SinkWorker>> fillers;
  for(int i = 0; i < 8; ++i)  // the most a channel holds
  {
    fillers.push_back(manual<LifecycleSink>(std::make_shared<Journal>()).worker);
    full->addDataSink(fillers.back());
  }
  auto holder = registry.getChannel("holder");
  holder->addDataSink(shared);  // already attached: must survive the undo
  auto plain = registry.getChannel("plain");
  auto plain2 = registry.getChannel("plain2");

  EXPECT_THROW(registry.addDefaultSink(shared), std::runtime_error);
  EXPECT_EQ(full->getNumberOfSinks(), 8u);
  EXPECT_EQ(holder->getNumberOfSinks(), 1u);
  EXPECT_EQ(plain->getNumberOfSinks(), 0u);
  EXPECT_EQ(plain2->getNumberOfSinks(), 0u);
  // Not a default sink either.
  EXPECT_EQ(registry.getChannel("after")->getNumberOfSinks(), 0u);
}

TEST(ChannelsRegistry, ChannelDefaultsApplyToNewChannelsOnly)
{
  ChannelsRegistry registry;
  double value = 1.0;
  auto existing = registry.getChannel("existing");

  ChannelDefaults defaults;
  defaults.pool_capacity = 3;
  registry.setChannelDefaults(defaults);

  auto sink = manual<LifecycleSink>(std::make_shared<Journal>()).worker;  // never drained
  registry.addDefaultSink(sink);
  auto created = registry.getChannel("created");
  EXPECT_EQ(registry.getChannel("created"), created) << "applied once, at creation";
  for(const auto& channel : { existing, created })
  {
    channel->registerValue("value", &value);
    channel->prepare();
  }
  EXPECT_EQ(acceptedUntilExhausted(*created), 3u);
  // An existing channel keeps the library default.
  EXPECT_EQ(acceptedUntilExhausted(*existing), 64u);

  // The pool in time: ceil(10 ms / 3 ms) = 4 slots.
  defaults = {};
  defaults.pool_stall_tolerance = milliseconds(10);
  defaults.pool_snapshot_period = milliseconds(3);
  registry.setChannelDefaults(defaults);
  auto timed = registry.getChannel("timed");
  timed->registerValue("value", &value);
  timed->prepare();
  EXPECT_EQ(acceptedUntilExhausted(*timed), 4u);

  // clear() forgets the defaults with the channels.
  registry.clear();
  auto fresh = registry.getChannel("fresh");
  fresh->registerValue("value", &value);
  fresh->addDataSink(sink);
  fresh->prepare();
  EXPECT_EQ(acceptedUntilExhausted(*fresh), 64u);
}

TEST(ChannelsRegistry, ChannelDefaultsPayloadCapacityIsAFloor)
{
  // A vector that grows after prepare(): tryTakeSnapshot() refuses the snapshot
  // when it outgrows the slot, unless the defaults reserved enough.
  for(const size_t floor : { size_t{ 0 }, size_t{ 4096 } })
  {
    ChannelsRegistry registry;
    ChannelDefaults defaults;
    defaults.payload_capacity = floor;
    registry.setChannelDefaults(defaults);
    registry.addDefaultSink(manual<LifecycleSink>(std::make_shared<Journal>()).worker);
    auto channel = registry.getChannel("payload");
    std::vector<double> values(1, 0.0);
    channel->registerValue("values", &values);
    channel->prepare();
    values.resize(400);  // 3200 bytes, beyond the automatic 256-byte reserve
    EXPECT_EQ(channel->tryTakeSnapshot(),
              floor == 0 ? SnapshotResult::oversize : SnapshotResult::ok)
        << "payload floor " << floor;
  }
}

TEST(ChannelsRegistry, ChannelDefaultsAreValidatedWhenSet)
{
  ChannelsRegistry registry;
  ChannelDefaults good;
  good.pool_capacity = 5;
  registry.setChannelDefaults(good);

  ChannelDefaults both = good;
  both.pool_stall_tolerance = milliseconds(10);
  both.pool_snapshot_period = milliseconds(1);
  EXPECT_THROW(registry.setChannelDefaults(both), std::invalid_argument);

  ChannelDefaults half;
  half.pool_stall_tolerance = milliseconds(10);
  EXPECT_THROW(registry.setChannelDefaults(half), std::invalid_argument);

  // A refused call keeps the previous defaults.
  double value = 1.0;
  registry.addDefaultSink(manual<LifecycleSink>(std::make_shared<Journal>()).worker);
  auto channel = registry.getChannel("validated");
  channel->registerValue("value", &value);
  channel->prepare();
  EXPECT_EQ(acceptedUntilExhausted(*channel), 5u);
}

TEST(ChannelsRegistry, StopAllStopsEverySinkItReachesOnce)
{
  ChannelsRegistry registry;
  double value = 1.0;
  auto default_journal = std::make_shared<Journal>();
  auto direct_journal = std::make_shared<Journal>();
  auto outside_journal = std::make_shared<Journal>();
  auto default_sink = attach<LifecycleSink>(Delivery::Threaded, default_journal).worker;
  auto direct_sink = attach<LifecycleSink>(Delivery::Threaded, direct_journal).worker;
  auto outside_sink = attach<LifecycleSink>(Delivery::Threaded, outside_journal).worker;

  registry.addDefaultSink(default_sink);
  auto first = registry.getChannel("first");
  auto second = registry.getChannel("second");
  // One sink attached directly to two registry channels.
  first->addDataSink(direct_sink);
  second->addDataSink(direct_sink);
  // A channel the registry does not know.
  auto outside = LogChannel::create("outside");
  outside->addDataSink(outside_sink);
  for(const auto& channel : { first, second, outside })
  {
    channel->registerValue("value", &value);
    for(int i = 0; i < 5; ++i)
    {
      ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
    }
  }

  registry.stopAll();
  {
    std::lock_guard lock(default_journal->mutex);
    EXPECT_EQ(default_journal->stops, 1);
    EXPECT_EQ(default_journal->snapshots_at_stop, 10);
    EXPECT_EQ(default_journal->stop_thread, std::this_thread::get_id());
  }
  {
    std::lock_guard lock(direct_journal->mutex);
    EXPECT_EQ(direct_journal->stops, 1);
    EXPECT_EQ(direct_journal->snapshots_at_stop, 10);
  }
  EXPECT_EQ(outside_journal->stopCount(), 0);
  EXPECT_EQ(first->takeSnapshot(), SnapshotResult::rejected);
  EXPECT_EQ(second->takeSnapshot(), SnapshotResult::rejected);
  EXPECT_EQ(outside->takeSnapshot(), SnapshotResult::ok);
  EXPECT_EQ(first->getNumberOfSinks(), 2u) << "sinks stay attached";

  registry.stopAll();  // idempotent
  EXPECT_EQ(default_journal->stopCount(), 1);
  EXPECT_EQ(direct_journal->stopCount(), 1);

  // A sink restarted on its own is stopped (and finished) again.
  direct_sink->start();
  ASSERT_EQ(first->takeSnapshot(), SnapshotResult::partial);  // default sink stopped
  registry.stopAll();
  EXPECT_EQ(direct_journal->stopCount(), 2);
  EXPECT_EQ(default_journal->stopCount(), 1);
}
