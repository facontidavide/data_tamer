#include "data_tamer/channel.hpp"
#include "data_tamer/details/snapshot_pool.hpp"
#include "data_tamer/sinks/mcap_sink.hpp"
#include "alloc_counter.hpp"
#include "mcap_test_utils.hpp"
#include "test_sinks.hpp"

#include <gtest/gtest.h>
#include <mcap/reader.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace DataTamer;
using DataTamerTest::attach;
using DataTamerTest::Attached;
using DataTamerTest::channelWith;
using DataTamerTest::manual;
using Delivery = SinkWorker::Delivery;

namespace
{
class QueueSink : public DataSink
{
public:
  void onSchema(const Schema&) override { ++registrations; }
  void onSnapshot(const SnapshotRef& snapshot) override
  {
    if(callback)
    {
      callback(snapshot);
    }
  }
  std::function<void(const SnapshotRef&)> callback;
  std::atomic<int> registrations{ 0 };
};

Attached<QueueSink> queueSink(Delivery delivery = Delivery::Manual)
{
  return attach<QueueSink>(delivery);
}
}  // namespace

TEST(SinkQueue, RetainedSnapshotOutlivesChannelAndQueue)
{
  SnapshotRef retained;
  uint64_t value = 42;
  {
    auto sink = queueSink();
    sink->callback = [&](const SnapshotRef& ref) { retained = ref.clone(); };
    auto channel = channelWith(sink, &value, std::string(128, 'n'));
    const auto hash = channel->getSchema().hash;
    ASSERT_EQ(channel->takeSnapshot(std::chrono::nanoseconds(123)), SnapshotResult::ok);
    channel.reset();  // Queued references must not depend on the channel.
    sink.drain();
    EXPECT_EQ(retained->schema_hash, hash);
  }
  ASSERT_TRUE(retained);
  EXPECT_EQ(retained->timestamp.count(), 123);
  ASSERT_EQ(retained->payload.size(), sizeof(value));
  uint64_t decoded = 0;
  std::memcpy(&decoded, retained->payload.data(), sizeof(decoded));
  EXPECT_EQ(decoded, 42u);
  EXPECT_TRUE(GetBit(retained->active_mask, 0));
}

TEST(SinkQueue, RefusedFanoutReleasesItsReferenceWithoutExhaustingPool)
{
  uint64_t value = 1;
  auto stopped = queueSink();
  auto running = queueSink();
  auto channel = channelWith(stopped, &value);
  channel->addDataSink(stopped);  // already attached: a no-op
  channel->addDataSink(running);
  int received = 0;
  running->callback = [&](const SnapshotRef&) { ++received; };
  for(int i = 0; i < 32; ++i)
  {
    ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
  }
  EXPECT_EQ(stopped->registrations.load(), 1);
  running.drain();
  stopped.worker->stop();  // delivers its 32, then refuses
  // Three times the pool: a refused clone that kept its slot would exhaust it.
  for(int i = 0; i < 192; ++i)
  {
    EXPECT_EQ(channel->takeSnapshot(), SnapshotResult::partial);
    running.drain();
  }
  EXPECT_EQ(received, 224);
  EXPECT_EQ(channel->droppedSnapshots(stopped), 192u);
  EXPECT_EQ(channel->droppedSnapshots(running), 0u);
  EXPECT_EQ(channel->poolExhausted(), 0u);
  stopped.worker->start();
  EXPECT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
  // Callbacks capture locals declared after the workers: stop before they die.
  stopped.worker->stop();
  running.worker->stop();
}

TEST(SinkQueue, PoolExhaustionIsSeparateAndRetainedSlotsAreReusable)
{
  uint64_t value = 1;
  auto sink = queueSink();
  auto channel = channelWith(sink, &value);
  std::vector<SnapshotRef> retained;
  sink->callback = [&](const SnapshotRef& ref) { retained.push_back(ref.clone()); };
  for(int i = 0; i < 64; ++i)
  {
    ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
    sink.drain();
  }
  EXPECT_NE(channel->takeSnapshot(), SnapshotResult::ok);
  EXPECT_EQ(channel->poolExhausted(), 1u);
  EXPECT_EQ(channel->stats().pool_exhausted, 1u);
  EXPECT_EQ(channel->droppedSnapshots(sink), 0u);
  retained.clear();
  EXPECT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
  channel->removeDataSink(sink);
  EXPECT_NE(channel->takeSnapshot(), SnapshotResult::ok);
  sink.worker->stop();  // delivers the last one while `retained` is still alive
}

TEST(SinkQueue, FixedPayloadFanoutAndRefusalDoNotAllocate)
{
  uint64_t value = 1;
  auto stopped = queueSink();
  auto running = queueSink();
  auto channel = channelWith(stopped, &value);
  channel->addDataSink(running);
  channel->prepare();  // Creates the pool and the queues, registers schemas.
  stopped.drain();     // The first delivery pass sizes the worker's pass list.
  running.drain();
  size_t allocations = 0, deallocations = 0;
  int successes = 0, partials = 0;
  {
    DataTamerTest::AllocCounter::Scope scope;
    for(int i = 0; i < 100; ++i)
    {
      successes += channel->tryTakeSnapshot() == SnapshotResult::ok;
      stopped.drain();
      running.drain();
    }
    stopped.worker->stop();  // Manual delivery: no thread to join
    for(int i = 0; i < 200; ++i)
    {
      partials += channel->tryTakeSnapshot() == SnapshotResult::partial;
      running.drain();
    }
    stopped.worker->start();
    for(int i = 0; i < 100; ++i)
    {
      successes += channel->tryTakeSnapshot() == SnapshotResult::ok;
      stopped.drain();
      running.drain();
    }
    allocations = scope.allocations();
    deallocations = scope.deallocations();
  }
  EXPECT_EQ(successes, 200);
  EXPECT_EQ(partials, 200);
  EXPECT_EQ(allocations, 0u);
  EXPECT_EQ(deallocations, 0u);
  EXPECT_EQ(channel->poolExhausted(), 0u);
}

TEST(SinkQueue, ExceptionsReleaseReferencesAndDoNotStopDelivery)
{
  for(bool worker : { false, true })
  {
    uint64_t value = 1;
    auto sink = queueSink(worker ? Delivery::Threaded : Delivery::Manual);
    auto channel = channelWith(sink, &value);
    std::mutex mutex;
    std::condition_variable delivered;
    int calls = 0;
    sink->callback = [&](const SnapshotRef&) {
      std::lock_guard lock(mutex);
      ++calls;
      delivered.notify_all();
      if(calls % 2)
      {
        throw std::runtime_error("callback failed");
      }
    };
    for(int i = 0; i < 160; ++i)
    {
      ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
      if(worker)
      {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(
            delivered.wait_for(lock, std::chrono::seconds(5), [&] { return calls > i; }));
      }
      else
      {
        sink.drain();
      }
    }
    sink.worker->stop();
    EXPECT_EQ(calls, 160);
    EXPECT_EQ(sink.worker->errors(), 80u);
    EXPECT_EQ(sink.worker->lastError(), "callback failed");
    EXPECT_EQ(channel->poolExhausted(), 0u);
  }
}

TEST(SinkQueue, WorkerAndManualDrainerSerializeCallbacksAndPreserveProducerOrder)
{
  auto sink = queueSink(Delivery::Threaded);
  uint64_t a = 0, b = 0;
  auto first = channelWith(sink, &a, "first");
  auto second = channelWith(sink, &b, "second");
  const auto first_hash = first->getSchema().hash;
  std::atomic<int> active{ 0 }, overlap{ 0 };
  std::vector<uint64_t> values[2];
  sink->callback = [&](const SnapshotRef& snapshot) {
    if(active.fetch_add(1) != 0)
    {
      ++overlap;
    }
    uint64_t value = 0;
    std::memcpy(&value, snapshot->payload.data(), sizeof(value));
    values[snapshot->schema_hash == first_hash ? 0 : 1].push_back(value);
    active.fetch_sub(1);
  };
  std::atomic<bool> done{ false };
  std::thread drainer([&] {
    while(!done)
    {
      sink.drain();
    }
  });
  std::vector<uint64_t> accepted[2];
  std::thread producer([&] {
    for(b = 0; b < 1000; ++b)
    {
      if(second->takeSnapshot() == SnapshotResult::ok)
      {
        accepted[1].push_back(b);
      }
    }
  });
  for(a = 0; a < 1000; ++a)
  {
    if(first->takeSnapshot() == SnapshotResult::ok)
    {
      accepted[0].push_back(a);
    }
  }
  producer.join();
  done = true;
  drainer.join();
  sink.worker->stop();
  EXPECT_EQ(overlap.load(), 0);
  EXPECT_EQ(values[0], accepted[0]);
  EXPECT_EQ(values[1], accepted[1]);
}

TEST(SinkQueue, RejectedWhenEverySinkRefusesPartialWhenSomeDo)
{
  uint64_t value = 1;
  auto first = queueSink(Delivery::Threaded);
  auto second = queueSink();
  auto channel = channelWith(first, &value);
  channel->addDataSink(second);
  ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
  first.worker->stop();  // admission closed: the only reason to refuse
  EXPECT_EQ(channel->takeSnapshot(), SnapshotResult::partial);
  second.worker->stop();
  EXPECT_EQ(channel->takeSnapshot(), SnapshotResult::rejected);
  EXPECT_EQ(channel->droppedSnapshots(first), 2u);
  EXPECT_EQ(channel->droppedSnapshots(second), 1u);
  first.worker->start();
  second.worker->start();
  EXPECT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
  first.worker->stop();
  second.worker->stop();
}

TEST(SinkQueue, WorkerDestructionDeliversQueuedSnapshotsBeforeTheSink)
{
  uint64_t value = 7;
  int delivered = 0;
  bool destroyed = false;
  struct Observer : DataSink
  {
    int& delivered;
    bool& destroyed;
    Observer(int& d, bool& x) : delivered(d), destroyed(x) {}
    ~Observer() override { destroyed = true; }
    void onSchema(const Schema&) override {}
    void onSnapshot(const SnapshotRef&) override
    {
      EXPECT_FALSE(destroyed);
      ++delivered;
    }
  };
  auto channel = LogChannel::create("observer");
  channel->registerValue("value", &value);
  {
    auto sink = manual<Observer>(delivered, destroyed);
    channel->addDataSink(sink);
    for(int i = 0; i < 5; ++i)
    {
      ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
    }
    channel->removeDataSink(sink);
  }
  EXPECT_TRUE(destroyed);
  EXPECT_EQ(delivered, 5);
}

TEST(SinkQueue, ConstructorExceptionPropagates)
{
  EXPECT_THROW(MCAPSink::create("/nonexistent/data_tamer_parent/file.mcap"),
               std::runtime_error);
  EXPECT_THROW(SinkWorker(nullptr), std::invalid_argument);
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
  auto sink = attach<MCAPSink>(Delivery::Threaded, first, true);
  auto channel = channelWith(sink, &value);
  for(const auto& path : { first, second })
  {
    if(path == second)
    {
      sink->restartRecording(path, true);
      sink.worker->start();
    }
    size_t accepted = 0;
    for(int i = 0; i < 1000; ++i)
    {
      if(channel->takeSnapshot() == SnapshotResult::ok)
      {
        ++accepted;
      }
    }
    for(int repeat = 0; repeat < 2; ++repeat)  // stopping twice must be harmless
    {
      sink.worker->stop();
      sink->stopRecording();
    }
    EXPECT_NE(channel->takeSnapshot(), SnapshotResult::ok);
    EXPECT_GT(accepted, 0u);
    EXPECT_EQ(DataTamerTest::countMessages(path), accepted);
  }
  std::filesystem::remove_all(directory);
}

TEST(SinkQueue, McapAutomaticRolloverDoesNotReopenClosedAcceptance)
{
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("data_tamer_rollover_" + std::to_string(NsecSinceEpoch().count()));
  std::filesystem::remove_all(directory);
  ASSERT_TRUE(std::filesystem::create_directory(directory));
  uint64_t value = 1;
  auto sink = manual<MCAPSink>((directory / "rollover.tamer.mcap").string());
  sink->setCreateNewFileOnReset(true);
  sink->setMaxTimeBeforeReset(std::chrono::seconds(-1));  // Every callback rolls over.
  auto channel = channelWith(sink, &value);
  for(int i = 0; i < 8; ++i)
  {
    ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
  }
  sink.worker->stop();  // delivers the 8, each one rolling the file over
  EXPECT_NE(channel->takeSnapshot(), SnapshotResult::ok);
  sink->stopRecording();
  size_t count = 0;
  size_t files = 0;
  for(const auto& file : std::filesystem::directory_iterator(directory))
  {
    // rollover.tamer.mcap, rollover_1.tamer.mcap, ...: the counter goes before
    // the extension, so the multi-part extension survives (#71, #98)
    const auto name = file.path().filename().string();
    EXPECT_EQ(name.substr(name.find('.')), ".tamer.mcap") << file.path();
    EXPECT_TRUE(name.rfind("rollover", 0) == 0) << file.path();
    mcap::McapReader reader;
    ASSERT_TRUE(reader.open(file.path().string()).ok());
    for(const auto& message : reader.readMessages())
    {
      // Sequence numbers restart at 1 in every file.
      EXPECT_EQ(message.message.sequence, 1u) << file.path();
      ++count;
    }
    ++files;
  }
  EXPECT_EQ(count, 8u);
  EXPECT_EQ(files, 9u);  // the last rollover leaves an empty file
  EXPECT_TRUE(std::filesystem::exists(directory / "rollover_1.tamer.mcap"));
  EXPECT_TRUE(std::filesystem::exists(directory / "rollover_8.tamer.mcap"));
  std::filesystem::remove_all(directory);
}

namespace
{
std::vector<uint32_t> mcapSequences(const std::filesystem::path& path)
{
  std::vector<uint32_t> sequences;
  mcap::McapReader reader;
  EXPECT_TRUE(reader.open(path.string()).ok()) << path;
  for(const auto& message : reader.readMessages())
  {
    sequences.push_back(message.message.sequence);
  }
  return sequences;
}
}  // namespace

TEST(SinkQueue, McapRollsOverOnResetByDefault)
{
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("data_tamer_default_rollover_" + std::to_string(NsecSinceEpoch().count()));
  std::filesystem::remove_all(directory);
  ASSERT_TRUE(std::filesystem::create_directory(directory));
  uint64_t value = 1;
  // Rolling over into new files is the default: no setCreateNewFileOnReset().
  auto sink = manual<MCAPSink>((directory / "default.mcap").string());
  sink->setMaxTimeBeforeReset(std::chrono::seconds(-1));  // Every callback rolls over.
  auto channel = channelWith(sink, &value);
  for(int i = 0; i < 4; ++i)
  {
    ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
  }
  sink.worker->stop();
  sink->stopRecording();
  size_t count = 0;
  size_t files = 0;
  for(const auto& file : std::filesystem::directory_iterator(directory))
  {
    count += mcapSequences(file.path()).size();
    ++files;
  }
  EXPECT_EQ(count, 4u);  // nothing was discarded
  EXPECT_EQ(files, 5u);  // the last rollover leaves an empty file
  EXPECT_TRUE(std::filesystem::exists(directory / "default.mcap"));
  EXPECT_TRUE(std::filesystem::exists(directory / "default_4.mcap"));
  std::filesystem::remove_all(directory);
}

TEST(SinkQueue, McapTruncatesOnResetWhenRolloverIsDisabled)
{
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("data_tamer_truncate_" + std::to_string(NsecSinceEpoch().count()));
  std::filesystem::remove_all(directory);
  ASSERT_TRUE(std::filesystem::create_directory(directory));
  uint64_t value = 1;
  auto sink = manual<MCAPSink>((directory / "truncate.mcap").string());
  sink->setCreateNewFileOnReset(false);  // opt in to truncating the file
  sink->setMaxTimeBeforeReset(std::chrono::seconds(-1));
  auto channel = channelWith(sink, &value);
  for(int i = 0; i < 4; ++i)
  {
    ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
  }
  sink.worker->stop();
  sink->stopRecording();
  size_t files = 0;
  for(const auto& file : std::filesystem::directory_iterator(directory))
  {
    EXPECT_EQ(file.path().filename(), "truncate.mcap");
    // each reset discarded what came before
    EXPECT_TRUE(mcapSequences(file.path()).empty());
    ++files;
  }
  EXPECT_EQ(files, 1u);
  std::filesystem::remove_all(directory);
}

TEST(SinkQueue, McapSequenceIsPerChannel)
{
  const auto path =
      (std::filesystem::temp_directory_path() /
       ("data_tamer_sequence_" + std::to_string(NsecSinceEpoch().count()) + ".mcap"))
          .string();
  uint64_t value = 1;
  auto sink = manual<MCAPSink>(path);
  auto first = channelWith(sink, &value, "first");
  auto second = channelWith(sink, &value, "second");
  for(int i = 0; i < 5; ++i)
  {
    ASSERT_EQ(first->takeSnapshot(), SnapshotResult::ok);
    if(i % 2 == 0)
    {
      ASSERT_EQ(second->takeSnapshot(), SnapshotResult::ok);
    }
  }
  sink.worker->stop();
  sink->stopRecording();
  std::map<std::string, std::vector<uint32_t>> sequences;
  {
    mcap::McapReader reader;
    ASSERT_TRUE(reader.open(path).ok());
    for(const auto& message : reader.readMessages())
    {
      sequences[message.channel->topic].push_back(message.message.sequence);
    }
  }
  std::filesystem::remove(path);
  EXPECT_EQ(sequences["first"], (std::vector<uint32_t>{ 1, 2, 3, 4, 5 }));
  EXPECT_EQ(sequences["second"], (std::vector<uint32_t>{ 1, 2, 3 }));
}

TEST(SinkQueue, McapNumberedPathKeepsMultiPartExtension)
{
  using details::NumberedPath;
  EXPECT_EQ(NumberedPath("run.tamer.mcap", 1), "run_1.tamer.mcap");
  EXPECT_EQ(NumberedPath("log.mcap", 3), "log_3.mcap");
  EXPECT_EQ(NumberedPath("log", 2), "log_2");
  EXPECT_EQ(NumberedPath("/tmp/dir.d/run.tamer.mcap", 4), "/tmp/dir.d/run_4.tamer.mcap");
  EXPECT_EQ(NumberedPath("dir.d/log", 1), "dir.d/log_1");
  EXPECT_EQ(NumberedPath("C:\\data.d\\log.mcap", 1), "C:\\data.d\\log_1.mcap");
  EXPECT_EQ(NumberedPath(".hidden", 1), ".hidden_1");
  EXPECT_EQ(NumberedPath("dir/.hidden.mcap", 2), "dir/.hidden_2.mcap");
  EXPECT_EQ(NumberedPath("./run.mcap", 1), "./run_1.mcap");
  // Dotted stems stay whole: only alphabetic trailing segments are the extension.
  EXPECT_EQ(NumberedPath("robot_v1.2.mcap", 1), "robot_v1.2_1.mcap");
  EXPECT_EQ(NumberedPath("log_2026.10.05.mcap", 1), "log_2026.10.05_1.mcap");
  EXPECT_EQ(NumberedPath("data.v2.mcap", 1), "data.v2_1.mcap");
  EXPECT_EQ(NumberedPath("log.123", 1), "log.123_1");
  EXPECT_EQ(NumberedPath("log.", 1), "log._1");
  EXPECT_EQ(NumberedPath("run.TAMER.Mcap", 1), "run_1.TAMER.Mcap");
}

TEST(SinkQueue, McapRolloverRestartsSequencePerFile)
{
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("data_tamer_rollover_seq_" + std::to_string(NsecSinceEpoch().count()));
  std::filesystem::remove_all(directory);
  ASSERT_TRUE(std::filesystem::create_directory(directory));
  uint64_t value = 1;
  auto sink = manual<MCAPSink>((directory / "seq.mcap").string());
  sink->setCreateNewFileOnReset(true);
  auto channel = channelWith(sink, &value);
  const auto take = [&](int count) {
    for(int i = 0; i < count; ++i)
    {
      ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
    }
    sink.drain();
  };
  sink->setMaxTimeBeforeReset(std::chrono::seconds(0));  // no rollover
  take(3);
  sink->setMaxTimeBeforeReset(std::chrono::seconds(-1));  // roll over after the next
  take(1);
  sink->setMaxTimeBeforeReset(std::chrono::seconds(0));
  take(3);
  sink.worker->stop();
  sink->stopRecording();
  EXPECT_EQ(mcapSequences(directory / "seq.mcap"), (std::vector<uint32_t>{ 1, 2, 3, 4 }));
  EXPECT_EQ(mcapSequences(directory / "seq_1.mcap"), (std::vector<uint32_t>{ 1, 2, 3 }));
  std::filesystem::remove_all(directory);
}

TEST(SinkQueue, McapRolloverSkipsExistingFiles)
{
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("data_tamer_rollover_skip_" + std::to_string(NsecSinceEpoch().count()));
  std::filesystem::remove_all(directory);
  ASSERT_TRUE(std::filesystem::create_directory(directory));
  // Leftovers of a previous run with the same path (with a gap at _3).
  for(const char* name : { "run_1.tamer.mcap", "run_2.tamer.mcap", "run_4.tamer.mcap" })
  {
    std::ofstream(directory / name) << "previous run";
  }
  uint64_t value = 1;
  auto sink = manual<MCAPSink>((directory / "run.tamer.mcap").string());
  sink->setCreateNewFileOnReset(true);
  sink->setMaxTimeBeforeReset(std::chrono::seconds(-1));
  auto channel = channelWith(sink, &value);
  for(int i = 0; i < 3; ++i)
  {
    ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
  }
  sink.worker->stop();
  sink->stopRecording();
  for(const char* name : { "run_1.tamer.mcap", "run_2.tamer.mcap", "run_4.tamer.mcap" })
  {
    std::ifstream file(directory / name);
    std::string content;
    std::getline(file, content);
    EXPECT_EQ(content, "previous run") << name;
  }
  // The three rollovers used the first unused names: _3, _5, _6.
  EXPECT_EQ(mcapSequences(directory / "run_3.tamer.mcap"), std::vector<uint32_t>{ 1 });
  EXPECT_EQ(mcapSequences(directory / "run_5.tamer.mcap"), std::vector<uint32_t>{ 1 });
  EXPECT_TRUE(mcapSequences(directory / "run_6.tamer.mcap").empty());
  EXPECT_FALSE(std::filesystem::exists(directory / "run_7.tamer.mcap"));
  std::filesystem::remove_all(directory);
}

TEST(SinkQueue, FastConsumerCannotReleaseParentBeforeSecondFanout)
{
  uint64_t value = 0;
  auto first = queueSink(Delivery::Threaded);
  auto second = queueSink(Delivery::Threaded);
  auto channel = channelWith(first, &value);
  channel->addDataSink(second);
  channel->setPoolCapacity(2);
  std::vector<uint64_t> received[2];
  for(int i = 0; i < 2; ++i)
  {
    auto& sink = i == 0 ? first : second;
    sink->callback = [&, i](const SnapshotRef& snapshot) {
      uint64_t decoded = 0;
      std::memcpy(&decoded, snapshot->payload.data(), sizeof(decoded));
      received[i].push_back(decoded);
    };
  }
  size_t accepted = 0;
  for(value = 0; value < 10000; ++value)
  {
    accepted += channel->takeSnapshot() == SnapshotResult::ok;
  }
  first.worker->stop();
  second.worker->stop();
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
  Attached<MCAPSink> sink(path, false);
  auto channel = channelWith(sink, &value);
  ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
  EXPECT_THROW(
      sink->restartRecording((directory / "missing" / "dir" / "x.mcap").string()),
      std::runtime_error);
  ASSERT_EQ(channel->takeSnapshot(),
            SnapshotResult::ok);  // still recording into the first file
  sink.worker->stop();
  sink->stopRecording();
  EXPECT_EQ(DataTamerTest::countMessages(path), 2u);
  std::filesystem::remove_all(directory);
}

namespace
{
/// Prepared channels on one sink, each logging its own counter, which take()
/// increments before every snapshot: delivered values grow in channel order.
struct CountingChannels
{
  CountingChannels(const std::shared_ptr<SinkWorker>& sink, size_t count, size_t pool)
    : counters(count, 0), values(count)
  {
    for(size_t i = 0; i < count; ++i)
    {
      auto channel = LogChannel::create("counting_" + std::to_string(i));
      channel->registerValue("counter", &counters[i]);
      channel->setPoolCapacity(pool);
      channel->addDataSink(sink);
      channel->prepare();
      hashes.push_back(channel->getSchema().hash);
      channels.push_back(channel);
    }
  }

  SnapshotResult take(size_t i)
  {
    ++counters[i];
    return channels[i]->tryTakeSnapshot();
  }

  /// Sink callback (deliverer only): files the snapshot's counter under its
  /// channel.
  void collect(const SnapshotRef& snapshot)
  {
    for(size_t i = 0; i < hashes.size(); ++i)
    {
      if(hashes[i] == snapshot->schema_hash)
      {
        uint64_t counter = 0;
        std::memcpy(&counter, snapshot->payload.data(), sizeof(counter));
        values[i].push_back(counter);
        return;
      }
    }
    ADD_FAILURE() << "snapshot of an unknown channel";
  }

  /// One producer thread per channel takes `snapshots` snapshots; returns how
  /// many each channel had accepted, and counts the refused ones (rejected or
  /// partial) in `refused`.
  std::vector<uint64_t> runProducers(uint64_t snapshots, std::vector<uint64_t>& refused)
  {
    std::vector<uint64_t> accepted(channels.size(), 0);
    refused.assign(channels.size(), 0);
    std::vector<std::thread> producers;
    for(size_t i = 0; i < channels.size(); ++i)
    {
      producers.emplace_back([&, i] {
        for(uint64_t n = 0; n < snapshots; ++n)
        {
          const auto result = take(i);
          accepted[i] += result == SnapshotResult::ok;
          refused[i] +=
              result == SnapshotResult::rejected || result == SnapshotResult::partial;
        }
      });
    }
    for(auto& producer : producers)
    {
      producer.join();
    }
    return accepted;
  }

  /// Channel `i` accepts exactly `free` snapshots, then finds its pool exhausted.
  void fillPool(size_t i, size_t free)
  {
    for(size_t n = 0; n < free; ++n)
    {
      ASSERT_EQ(take(i), SnapshotResult::ok) << "channel " << i << " snapshot " << n;
    }
    ASSERT_EQ(take(i), SnapshotResult::pool_exhausted) << "channel " << i;
  }

  /// Every channel had `expected[i]` snapshots collected, in the order they
  /// were taken, each once; then forgets them.
  void verify(const std::vector<uint64_t>& expected)
  {
    for(size_t i = 0; i < channels.size(); ++i)
    {
      EXPECT_EQ(values[i].size(), expected[i]) << "channel " << i;
      for(size_t k = 1; k < values[i].size(); ++k)
      {
        ASSERT_LT(values[i][k - 1], values[i][k]) << "channel " << i << " entry " << k;
      }
      values[i].clear();
    }
  }

  std::vector<uint64_t> counters;  // sized once: channels hold pointers into it
  std::vector<uint64_t> hashes;
  std::vector<std::shared_ptr<LogChannel>> channels;
  std::vector<std::vector<uint64_t>> values;  // collect() only
};
}  // namespace

// The pool is the only bound on what a channel can have in flight to a sink:
// each channel takes every free slot of its pool, then finds the pool
// exhausted, never a full queue. Several channels share the worker; every
// round first moves the queue positions by `pad`, so that the queues wrap at
// every offset; detaching and attaching again with snapshots still queued
// changes nothing, and keeps per-channel order.
TEST(SinkQueue, PoolIsTheOnlyBoundAcrossChannelsRoundsAndReattachment)
{
  constexpr size_t kChannels = 4;
  constexpr size_t kPool = 300;
  auto sink = queueSink();
  CountingChannels counting(sink, kChannels, kPool);
  size_t delivered = 0;
  sink->callback = [&](const SnapshotRef&) { ++delivered; };
  sink.drain();  // sizes the worker's pass list; nothing is queued yet
  size_t allocations = 0, deallocations = 0;
  {
    DataTamerTest::AllocCounter::Scope scope;
    for(size_t pad = 0; pad < 64; ++pad)
    {
      for(size_t n = 0; n < pad; ++n)
      {
        for(size_t i = 0; i < kChannels; ++i)
        {
          ASSERT_EQ(counting.take(i), SnapshotResult::ok);
        }
      }
      sink.drain();
      for(size_t i = 0; i < kChannels; ++i)
      {
        counting.fillPool(i, kPool);
        ASSERT_FALSE(testing::Test::HasFatalFailure()) << "pad " << pad;
      }
      sink.drain();
    }
    allocations = scope.allocations();
    deallocations = scope.deallocations();
  }
  EXPECT_EQ(allocations, 0u);
  EXPECT_EQ(deallocations, 0u);
  EXPECT_EQ(delivered, kChannels * (64 * kPool + 63 * 64 / 2));  // pads 0..63

  sink->callback = [&](const SnapshotRef& snapshot) { counting.collect(snapshot); };
  constexpr size_t kQueued = 100;
  for(size_t n = 0; n < kQueued; ++n)
  {
    for(size_t i = 0; i < kChannels; ++i)
    {
      ASSERT_EQ(counting.take(i), SnapshotResult::ok);
    }
  }
  // Two channels leave and come back with 100 snapshots queued in the old queue,
  // which still hold their slots: the pool has 200 free, and so does each queue.
  for(size_t i = 0; i < 2; ++i)
  {
    counting.channels[i]->removeDataSink(sink);
    counting.channels[i]->addDataSink(sink);
  }
  for(size_t i = 0; i < kChannels; ++i)
  {
    counting.fillPool(i, kPool - kQueued);
  }
  sink.drain();  // the detached queues first, then the new ones
  const std::vector<uint64_t> whole_pools(kChannels, kPool);
  counting.verify(whole_pools);
  // The old queues are gone; the new ones carry a whole pool again.
  for(size_t i = 0; i < kChannels; ++i)
  {
    counting.fillPool(i, kPool);
  }
  sink.drain();
  counting.verify(whole_pools);
  for(size_t i = 0; i < kChannels; ++i)
  {
    EXPECT_EQ(counting.channels[i]->droppedSnapshots(sink), 0u);
  }
}

// A control thread detaches and attaches the sink of three channels as fast as
// it can, and drains alongside the worker thread, while each channel logs from
// its own thread. Nothing is refused while the worker runs (the pool is the
// only bound), every accepted snapshot is delivered exactly once, and each
// channel's snapshots arrive in order. Meant to run under ThreadSanitizer.
TEST(SinkQueue, AttachAndDetachWhileLoggingKeepEveryAcceptedSnapshotInOrder)
{
  constexpr size_t kChannels = 3;
  auto sink = queueSink(Delivery::Threaded);
  CountingChannels counting(sink, kChannels, 16);
  // Before the first snapshot, so before the first callback.
  sink->callback = [&](const SnapshotRef& snapshot) { counting.collect(snapshot); };
  std::atomic<bool> done{ false };
  std::thread control([&] {
    while(!done)
    {
      for(const auto& channel : counting.channels)
      {
        channel->removeDataSink(sink);
        channel->addDataSink(sink);
      }
      sink.drain();
    }
  });
  std::vector<uint64_t> refused;
  const auto accepted = counting.runProducers(20000, refused);
  done = true;
  control.join();
  sink.worker->stop();
  for(size_t i = 0; i < kChannels; ++i)
  {
    EXPECT_EQ(refused[i], 0u) << "channel " << i;
    EXPECT_GT(accepted[i], 0u) << "channel " << i;
  }
  counting.verify(accepted);
}

// removeDataSink() and addDataSink() of the same worker run on two threads
// while each channel logs from its own: the queue a removal leaves behind is
// delivered before the queue of the next attachment, so every channel's
// snapshots still arrive in order, and every accepted one exactly once.
TEST(SinkQueue, RemoveAndAddOnDifferentThreadsKeepPerChannelOrder)
{
  constexpr size_t kChannels = 2;
  auto sink = queueSink(Delivery::Threaded);
  CountingChannels counting(sink, kChannels, 32);
  sink->callback = [&](const SnapshotRef& snapshot) { counting.collect(snapshot); };
  std::atomic<bool> done{ false };
  std::thread remover([&] {
    while(!done)
    {
      for(const auto& channel : counting.channels)
      {
        channel->removeDataSink(sink);
      }
    }
  });
  std::thread adder([&] {
    while(!done)
    {
      for(const auto& channel : counting.channels)
      {
        channel->addDataSink(sink);
      }
    }
  });
  std::vector<uint64_t> refused;  // a removed sink refuses nothing: not checked
  const auto accepted = counting.runProducers(20000, refused);
  done = true;
  remover.join();
  adder.join();
  sink.worker->stop();
  counting.verify(accepted);
}

namespace
{
// Refuses onSchema() while `refuse` is set.
class RefusingSink : public DataSink
{
public:
  void onSchema(const Schema&) override
  {
    if(refuse)
    {
      throw std::runtime_error("schema refused");
    }
  }
  void onSnapshot(const SnapshotRef&) override {}
  std::atomic<bool> refuse{ false };
};
}  // namespace

// A queue attached while the worker is inside a delivery pass is served
// without another push: that pass reports itself incomplete and the worker
// runs the next one at once. Channel B's queue lands on the worker during the
// gated callback of channel A: B's prepare() announced B to the worker and then
// failed on another sink, so the retry announces only to that other sink (no
// worker lock needed) and attaches B's queue. B's push posts before the gate
// opens, so the last round of A's pass reads that post: without the check
// that the attachment list grew, the worker would sleep with B's snapshot
// queued.
TEST(SinkQueue, QueueAttachedDuringAPassIsServedWithoutAnotherPush)
{
  auto sink = queueSink(Delivery::Threaded);
  std::mutex mutex;
  std::condition_variable condition;
  bool entered = false, released = false;
  std::vector<uint64_t> delivered;  // schema hashes
  sink->callback = [&](const SnapshotRef& snapshot) {
    std::unique_lock lock(mutex);
    delivered.push_back(snapshot->schema_hash);
    if(!entered)
    {
      entered = true;
      condition.notify_all();
      condition.wait(lock, [&] { return released; });
    }
    condition.notify_all();
  };
  uint64_t a_value = 1, b_value = 2;
  auto a = channelWith(sink, &a_value, "during_pass_a");
  a->prepare();

  // Slots are filled from the highest down and announced from the lowest up:
  // `sink` hears B's schema before `refusing` throws.
  auto refusing = manual<RefusingSink>();
  auto b = LogChannel::create("during_pass_b");
  b->registerValue("value", &b_value);
  b->addDataSink(refusing);
  b->addDataSink(sink);
  refusing->refuse = true;
  EXPECT_THROW(b->prepare(), std::runtime_error);
  refusing->refuse = false;
  const auto b_hash = b->getSchema().hash;

  ASSERT_EQ(a->tryTakeSnapshot(), SnapshotResult::ok);
  {
    std::unique_lock lock(mutex);
    ASSERT_TRUE(
        condition.wait_for(lock, std::chrono::seconds(5), [&] { return entered; }));
  }
  b->prepare();  // attaches B's queue on `sink` while its pass is gated
  ASSERT_EQ(b->tryTakeSnapshot(), SnapshotResult::ok);
  {
    std::lock_guard lock(mutex);
    released = true;
  }
  condition.notify_all();
  {
    std::unique_lock lock(mutex);
    EXPECT_TRUE(condition.wait_for(lock, std::chrono::seconds(5), [&] {
      return std::find(delivered.begin(), delivered.end(), b_hash) != delivered.end();
    })) << "B's snapshot waited for another push";
  }
  sink.worker->stop();
}

// A detached channel's queue is freed by the delivery pass that empties it,
// not when the worker is destroyed: here a Manual worker's drain() frees the
// ring's entries and its list node, once.
TEST(SinkQueue, DrainFreesTheQueueOfADetachedChannel)
{
  uint64_t value = 1;
  auto sink = queueSink();
  int delivered = 0;
  sink->callback = [&](const SnapshotRef&) { ++delivered; };
  auto channel = channelWith(sink, &value);
  channel->prepare();
  for(int i = 0; i < 5; ++i)
  {
    ASSERT_EQ(channel->tryTakeSnapshot(), SnapshotResult::ok);
  }
  channel->removeDataSink(sink);  // frees the link; the queue keeps its 5
  size_t freed = 0;
  {
    DataTamerTest::AllocCounter::Scope scope;
    sink.drain();
    freed = scope.deallocations();
  }
  EXPECT_EQ(delivered, 5);
  EXPECT_EQ(freed, 2u);
  {
    DataTamerTest::AllocCounter::Scope scope;
    sink.drain();
    freed = scope.deallocations();
  }
  EXPECT_EQ(freed, 0u);
  // An empty queue goes at the next pass too.
  channel->addDataSink(sink);
  channel->removeDataSink(sink);
  {
    DataTamerTest::AllocCounter::Scope scope;
    sink.drain();
    freed = scope.deallocations();
  }
  EXPECT_EQ(freed, 2u);
  EXPECT_EQ(delivered, 5);
}

// Lost wake-up stress: one snapshot at a time to a Threaded worker, each
// waited for with a timeout. The pauses before each push straddle the worker's
// polling window (about 20 us), so pushes land while it polls, while it marks
// itself asleep and while it sleeps. Not deterministic: a lost wake-up shows as
// a timeout on some round. Meant to run under ThreadSanitizer too.
TEST(SinkQueue, EverySnapshotWakesAnIdleWorker)
{
  uint64_t value = 0;
  auto sink = queueSink(Delivery::Threaded);
  std::mutex mutex;
  std::condition_variable condition;
  uint64_t delivered = 0;
  sink->callback = [&](const SnapshotRef&) {
    {
      std::lock_guard lock(mutex);
      ++delivered;
    }
    condition.notify_all();
  };
  auto channel = channelWith(sink, &value);
  channel->prepare();
  using std::chrono::microseconds;
  const std::array<microseconds, 6> pauses{ microseconds(0),  microseconds(5),
                                            microseconds(18), microseconds(22),
                                            microseconds(40), microseconds(100) };
  constexpr uint64_t kRounds = 20000;
  for(uint64_t n = 1; n <= kRounds; ++n)
  {
    const auto until = std::chrono::steady_clock::now() + pauses[n % pauses.size()];
    while(std::chrono::steady_clock::now() < until)
    {
    }
    ASSERT_EQ(channel->tryTakeSnapshot(), SnapshotResult::ok);
    std::unique_lock lock(mutex);
    ASSERT_TRUE(
        condition.wait_for(lock, std::chrono::seconds(5), [&] { return delivered == n; }))
        << "round " << n;
  }
  sink.worker->stop();
}
