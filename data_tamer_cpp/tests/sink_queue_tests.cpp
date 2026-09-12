#include "data_tamer/channel.hpp"
#include "data_tamer/details/snapshot_pool.hpp"
#include "data_tamer/sinks/mcap_sink.hpp"
#include "alloc_counter.hpp"

#include <gtest/gtest.h>
#include <mcap/reader.hpp>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>

using namespace DataTamer;

namespace
{
class QueueSink : public DataSinkBase
{
public:
  explicit QueueSink(size_t capacity = 1024, bool worker = false) : DataSinkBase(capacity)
  {
    if(!worker)
      stopThread();
  }
  ~QueueSink() override { stopThread(); }
  using DataSinkBase::processQueuedSnapshots;
  using DataSinkBase::retainSnapshot;
  using DataSinkBase::startAcceptingSnapshots;
  using DataSinkBase::stopAcceptingSnapshots;
  using DataSinkBase::stopThread;
  void addChannel(const std::string&, const Schema&) override { ++registrations; }
  bool storeSnapshot(const Snapshot& snapshot) override
  {
    return callback ? callback(snapshot) : true;
  }
  std::function<bool(const Snapshot&)> callback;
  std::atomic<int> registrations{ 0 };
};

std::shared_ptr<LogChannel> channelWith(const std::shared_ptr<DataSinkBase>& sink,
                                        const uint64_t* value,
                                        const std::string& name = "queue_test")
{
  auto channel = LogChannel::create(name);
  channel->registerValue("value", value);
  channel->addDataSink(sink);
  return channel;
}
}  // namespace

TEST(SinkQueue, RetainedSnapshotOutlivesChannelAndQueue)
{
  SnapshotRef retained;
  uint64_t value = 42;
  {
    auto sink = std::make_shared<QueueSink>();
    sink->callback = [&](const Snapshot&) {
      retained = sink->retainSnapshot();
      return true;
    };
    auto channel = channelWith(sink, &value, std::string(128, 'n'));
    ASSERT_TRUE(channel->takeSnapshot(std::chrono::nanoseconds(123)));
    channel.reset();  // Even queued references must own the name.
    sink->processQueuedSnapshots();
  }
  ASSERT_TRUE(retained);
  EXPECT_EQ(retained->channel_name, std::string(128, 'n'));
  EXPECT_EQ(retained->timestamp.count(), 123);
  ASSERT_EQ(retained->payload.size(), sizeof(value));
  uint64_t decoded = 0;
  std::memcpy(&decoded, retained->payload.data(), sizeof(decoded));
  EXPECT_EQ(decoded, 42u);
  EXPECT_TRUE(GetBit(retained->active_mask, 0));
}

TEST(SinkQueue, QueueOverflowReleasesFailedFanoutWithoutExhaustingPool)
{
  uint64_t value = 1;
  auto small = std::make_shared<QueueSink>(1);
  auto large = std::make_shared<QueueSink>();
  auto channel = channelWith(small, &value);
  channel->addDataSink(small);  // Must keep the existing producer token.
  channel->addDataSink(large);
  int received = 0;
  large->callback = [&](const Snapshot&) {
    ++received;
    return true;
  };
  for(int i = 0; i < 32; ++i)
    ASSERT_TRUE(channel->takeSnapshot());
  EXPECT_EQ(small->registrations.load(), 1);
  large->processQueuedSnapshots();
  for(int i = 0; i < 160; ++i)
  {
    EXPECT_FALSE(channel->takeSnapshot());
    large->processQueuedSnapshots();
  }
  EXPECT_EQ(received, 192);
  EXPECT_EQ(channel->droppedSnapshots(small), 160u);
  EXPECT_EQ(channel->droppedSnapshots(large), 0u);
  EXPECT_EQ(channel->poolExhausted(), 0u);
  small->processQueuedSnapshots();
  EXPECT_TRUE(channel->takeSnapshot());
}

TEST(SinkQueue, PoolExhaustionIsSeparateAndRetainedSlotsAreReusable)
{
  uint64_t value = 1;
  auto sink = std::make_shared<QueueSink>();
  auto channel = channelWith(sink, &value);
  std::vector<SnapshotRef> retained;
  sink->callback = [&](const Snapshot&) {
    retained.push_back(sink->retainSnapshot());
    return true;
  };
  for(int i = 0; i < 64; ++i)
  {
    ASSERT_TRUE(channel->takeSnapshot());
    sink->processQueuedSnapshots();
  }
  EXPECT_FALSE(channel->takeSnapshot());
  EXPECT_EQ(channel->poolExhausted(), 1u);
  EXPECT_EQ(channel->stats().pool_exhausted, 1u);
  EXPECT_EQ(channel->droppedSnapshots(sink), 0u);
  retained.clear();
  EXPECT_TRUE(channel->takeSnapshot());
  channel->removeDataSink(sink);
  EXPECT_FALSE(channel->takeSnapshot());
}

TEST(SinkQueue, FixedPayloadFanoutAndOverflowDoNotAllocate)
{
  uint64_t value = 1;
  auto small = std::make_shared<QueueSink>(1);
  auto large = std::make_shared<QueueSink>();
  auto channel = channelWith(small, &value);
  channel->addDataSink(large);
  ASSERT_TRUE(channel->takeSnapshot());  // Creates the pool and registers schemas.
  size_t allocations = 0, deallocations = 0;
  int successes = 0, failures = 0;
  {
    DataTamerTest::AllocCounter::Scope scope;
    for(int i = 0; i < 200; ++i)
    {
      if(channel->takeSnapshot())
        ++successes;
      else
        ++failures;
      large->processQueuedSnapshots();
    }
    small->processQueuedSnapshots();
    for(int i = 0; i < 100; ++i)
    {
      if(channel->takeSnapshot())
        ++successes;
      small->processQueuedSnapshots();
      large->processQueuedSnapshots();
    }
    allocations = scope.allocations();
    deallocations = scope.deallocations();
  }
  EXPECT_GT(successes, 100);
  EXPECT_GT(failures, 100);
  EXPECT_EQ(allocations, 0u);
  EXPECT_EQ(deallocations, 0u);
  EXPECT_EQ(channel->poolExhausted(), 0u);
}

TEST(SinkQueue, ExceptionsReleaseReferencesAndDoNotStopDelivery)
{
  for(bool worker : { false, true })
  {
    uint64_t value = 1;
    auto sink = std::make_shared<QueueSink>(1024, worker);
    auto channel = channelWith(sink, &value);
    std::mutex mutex;
    std::condition_variable delivered;
    int calls = 0;
    sink->callback = [&](const Snapshot&) {
      std::lock_guard lock(mutex);
      ++calls;
      delivered.notify_all();
      if(calls % 2)
        throw std::runtime_error("callback failed");
      return false;  // A false result is not an exception.
    };
    for(int i = 0; i < 160; ++i)
    {
      ASSERT_TRUE(channel->takeSnapshot());
      if(worker)
      {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(
            delivered.wait_for(lock, std::chrono::seconds(5), [&] { return calls > i; }));
      }
      else
        sink->processQueuedSnapshots();
    }
    sink->stopThread();
    sink->processQueuedSnapshots();
    EXPECT_EQ(calls, 160);
    EXPECT_EQ(sink->storeErrors(), 80u);
    EXPECT_EQ(channel->poolExhausted(), 0u);
  }
}

TEST(SinkQueue, WorkerAndManualDrainerSerializeCallbacksAndPreserveProducerOrder)
{
  auto sink = std::make_shared<QueueSink>(1024, true);
  uint64_t a = 0, b = 0;
  auto first = channelWith(sink, &a, "first");
  auto second = channelWith(sink, &b, "second");
  std::atomic<int> active{ 0 }, overlap{ 0 };
  std::vector<uint64_t> values[2];
  sink->callback = [&](const Snapshot& snapshot) {
    if(active.fetch_add(1) != 0)
      ++overlap;
    uint64_t value = 0;
    std::memcpy(&value, snapshot.payload.data(), sizeof(value));
    values[snapshot.channel_name == "first" ? 0 : 1].push_back(value);
    active.fetch_sub(1);
    return true;
  };
  std::atomic<bool> done{ false };
  std::thread drainer([&] {
    while(!done)
      sink->processQueuedSnapshots();
  });
  std::vector<uint64_t> accepted[2];
  std::thread producer([&] {
    for(b = 0; b < 1000; ++b)
      if(second->takeSnapshot())
        accepted[1].push_back(b);
  });
  for(a = 0; a < 1000; ++a)
    if(first->takeSnapshot())
      accepted[0].push_back(a);
  producer.join();
  done = true;
  drainer.join();
  sink->stopThread();
  sink->processQueuedSnapshots();
  EXPECT_EQ(overlap.load(), 0);
  EXPECT_EQ(values[0], accepted[0]);
  EXPECT_EQ(values[1], accepted[1]);
}

TEST(SinkQueue, CloseDuringPublicationDrainsEveryAcceptedSnapshot)
{
  auto sink = std::make_shared<QueueSink>(1024, true);
  uint64_t value = 0;
  auto channel = channelWith(sink, &value);
  std::mutex mutex;
  std::condition_variable ready;
  bool entered = false, release = false;
  int stored = 0;
  sink->callback = [&](const Snapshot&) {
    std::unique_lock lock(mutex);
    ++stored;
    if(stored == 1)
    {
      entered = true;
      ready.notify_all();
      ready.wait(lock, [&] { return release; });
    }
    return true;
  };
  ASSERT_TRUE(channel->takeSnapshot());
  {
    std::unique_lock lock(mutex);
    ASSERT_TRUE(ready.wait_for(lock, std::chrono::seconds(5), [&] { return entered; }));
  }
  ASSERT_TRUE(channel->takeSnapshot());  // Guaranteed accepted work remains queued.
  std::atomic<bool> publish{ true }, attempted{ false };
  int accepted = 2;
  std::thread producer([&] {
    while(publish)
    {
      if(channel->takeSnapshot())
        ++accepted;
      attempted = true;
    }
  });
  while(!attempted)
    std::this_thread::yield();
  sink->stopAcceptingSnapshots();  // Must return even though callback is blocked.
  publish = false;
  producer.join();
  EXPECT_FALSE(channel->takeSnapshot());
  {
    std::lock_guard lock(mutex);
    release = true;
  }
  ready.notify_all();
  sink->stopThread();
  sink->processQueuedSnapshots();
  EXPECT_EQ(stored, accepted);
  sink->startAcceptingSnapshots();
  EXPECT_TRUE(channel->takeSnapshot());
  sink->processQueuedSnapshots();
  EXPECT_EQ(stored, accepted + 1);
}

#ifndef NDEBUG
TEST(SinkQueueDeathTest, DerivedDestructorMustStopWorker)
{
  EXPECT_DEATH(
      {
        class BrokenSink : public DataSinkBase
        {
          void addChannel(const std::string&, const Schema&) override {}
          bool storeSnapshot(const Snapshot&) override { return true; }
        } sink;
      },
      "stopThread");
}
#endif

TEST(SinkQueue, ConstructorExceptionPropagates)
{
  EXPECT_THROW(MCAPSink("/nonexistent/data_tamer_parent/file.mcap"), std::runtime_error);
}

TEST(SinkQueue, McapFinalizationWritesEveryAcceptedSnapshotAfterRestart)
{
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("data_tamer_sink_queue_" + std::to_string(NsecSinceEpoch().count()));
  std::filesystem::remove_all(directory);
  ASSERT_TRUE(std::filesystem::create_directory(directory));
  const auto first = (directory / "first.mcap").string();
  const auto second = (directory / "second.mcap").string();
  uint64_t value = 9;
  auto sink = std::make_shared<MCAPSink>(first, true, 1);
  auto channel = channelWith(sink, &value);
  for(const auto& path : { first, second })
  {
    if(path == second)
      sink->restartRecording(path, true);
    size_t accepted = 0;
    for(int i = 0; i < 1000; ++i)
      if(channel->takeSnapshot())
        ++accepted;
    sink->finishQueueAndStop();
    sink->finishQueueAndStop();  // stopping twice must be harmless
    EXPECT_FALSE(channel->takeSnapshot());
    mcap::McapReader reader;
    ASSERT_TRUE(reader.open(path).ok());
    size_t count = 0;
    for(const auto& message : reader.readMessages())
    {
      (void)message;
      ++count;
    }
    EXPECT_GT(accepted, 0u);
    EXPECT_EQ(count, accepted);
  }
  std::filesystem::remove_all(directory);
}

TEST(SinkQueue, McapAutomaticRolloverDoesNotReopenClosedAcceptance)
{
  class ControlledMcap : public MCAPSink
  {
  public:
    explicit ControlledMcap(const std::string& path) : MCAPSink(path) { stopThread(); }
    using DataSinkBase::processQueuedSnapshots;
    using DataSinkBase::stopAcceptingSnapshots;
  };
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("data_tamer_rollover_" + std::to_string(NsecSinceEpoch().count()));
  std::filesystem::remove_all(directory);
  ASSERT_TRUE(std::filesystem::create_directory(directory));
  uint64_t value = 1;
  auto sink = std::make_shared<ControlledMcap>((directory / "rollover.mcap").string());
  sink->setCreateNewFileOnReset(true);
  sink->setMaxTimeBeforeReset(std::chrono::seconds(-1));  // Every callback rolls over.
  auto channel = channelWith(sink, &value);
  for(int i = 0; i < 8; ++i)
    ASSERT_TRUE(channel->takeSnapshot());
  sink->stopAcceptingSnapshots();
  sink->processQueuedSnapshots();
  EXPECT_FALSE(channel->takeSnapshot());
  sink->finishQueueAndStop();
  size_t count = 0;
  for(const auto& file : std::filesystem::directory_iterator(directory))
  {
    mcap::McapReader reader;
    ASSERT_TRUE(reader.open(file.path().string()).ok());
    for(const auto& message : reader.readMessages())
    {
      (void)message;
      ++count;
    }
  }
  EXPECT_EQ(count, 8u);
  std::filesystem::remove_all(directory);
}

TEST(SinkQueue, FastConsumerCannotReleaseParentBeforeSecondFanout)
{
  uint64_t value = 0;
  auto first = std::make_shared<QueueSink>(1024, true);
  auto second = std::make_shared<QueueSink>(1024, true);
  auto channel = channelWith(first, &value);
  channel->addDataSink(second);
  channel->setPoolCapacity(2);
  std::vector<uint64_t> received[2];
  for(int i = 0; i < 2; ++i)
  {
    auto sink = i == 0 ? first : second;
    sink->callback = [&, i](const Snapshot& snapshot) {
      uint64_t decoded = 0;
      std::memcpy(&decoded, snapshot.payload.data(), sizeof(decoded));
      received[i].push_back(decoded);
      return true;
    };
  }
  size_t accepted = 0;
  for(value = 0; value < 10000; ++value)
    accepted += channel->takeSnapshot();
  first->stopThread();
  second->stopThread();
  first->processQueuedSnapshots();
  second->processQueuedSnapshots();
  EXPECT_GT(accepted, 0u);
  EXPECT_EQ(channel->droppedSnapshots(first), 0u);
  EXPECT_EQ(channel->droppedSnapshots(second), 0u);
  EXPECT_EQ(received[0].size(), accepted);
  EXPECT_EQ(received[0], received[1]);
}

// A restart whose file cannot be opened must throw and leave the current
// recording running: the old writer is replaced only after the new one opened.
TEST(SinkQueue, McapFailedRestartKeepsTheCurrentRecording)
{
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("data_tamer_restart_" + std::to_string(NsecSinceEpoch().count()));
  std::filesystem::remove_all(directory);
  ASSERT_TRUE(std::filesystem::create_directory(directory));
  const auto path = (directory / "log.mcap").string();
  uint64_t value = 3;
  auto sink = std::make_shared<MCAPSink>(path, false);
  auto channel = channelWith(sink, &value);
  ASSERT_TRUE(channel->takeSnapshot());
  EXPECT_THROW(
      sink->restartRecording((directory / "missing" / "dir" / "x.mcap").string()),
      std::runtime_error);
  ASSERT_TRUE(channel->takeSnapshot());  // still recording into the first file
  sink->finishQueueAndStop();
  mcap::McapReader reader;
  ASSERT_TRUE(reader.open(path).ok());
  size_t count = 0;
  for(const auto& message : reader.readMessages())
  {
    (void)message;
    ++count;
  }
  EXPECT_EQ(count, 2u);
  reader.close();
  std::filesystem::remove_all(directory);
}
