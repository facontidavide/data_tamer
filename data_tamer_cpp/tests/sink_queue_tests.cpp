#include "data_tamer/channel.hpp"
#include "data_tamer/details/snapshot_pool.hpp"
#include "data_tamer/sinks/mcap_sink.hpp"
#include "alloc_counter.hpp"
#include "test_sinks.hpp"

#include <gtest/gtest.h>
#include <mcap/reader.hpp>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

using namespace DataTamer;
using DataTamerTest::attach;
using DataTamerTest::Attached;
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

Attached<QueueSink> queueSink(size_t capacity = 1024,
                              Delivery delivery = Delivery::Manual)
{
  return attach<QueueSink>(delivery, capacity);
}

std::shared_ptr<LogChannel> channelWith(const std::shared_ptr<SinkWorker>& sink,
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

TEST(SinkQueue, QueueOverflowReleasesFailedFanoutWithoutExhaustingPool)
{
  uint64_t value = 1;
  auto small = queueSink(1);
  auto large = queueSink();
  auto channel = channelWith(small, &value);
  channel->addDataSink(small);  // Must keep the existing producer token.
  channel->addDataSink(large);
  int received = 0;
  large->callback = [&](const SnapshotRef&) { ++received; };
  for(int i = 0; i < 32; ++i)
  {
    ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
  }
  EXPECT_EQ(small->registrations.load(), 1);
  large.drain();
  for(int i = 0; i < 160; ++i)
  {
    EXPECT_EQ(channel->takeSnapshot(), SnapshotResult::partial);
    large.drain();
  }
  EXPECT_EQ(received, 192);
  EXPECT_EQ(channel->droppedSnapshots(small), 160u);
  EXPECT_EQ(channel->droppedSnapshots(large), 0u);
  EXPECT_EQ(channel->poolExhausted(), 0u);
  small.drain();
  EXPECT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
  // Callbacks capture locals declared after the workers: stop before they die.
  small.worker->stop();
  large.worker->stop();
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

TEST(SinkQueue, FixedPayloadFanoutAndOverflowDoNotAllocate)
{
  uint64_t value = 1;
  auto small = queueSink(1);
  auto large = queueSink();
  auto channel = channelWith(small, &value);
  channel->addDataSink(large);
  ASSERT_EQ(channel->takeSnapshot(),
            SnapshotResult::ok);  // Creates the pool and registers schemas.
  size_t allocations = 0, deallocations = 0;
  int successes = 0, failures = 0;
  {
    DataTamerTest::AllocCounter::Scope scope;
    for(int i = 0; i < 200; ++i)
    {
      if(channel->takeSnapshot() == SnapshotResult::ok)
      {
        ++successes;
      }
      else
      {
        ++failures;
      }
      large.drain();
    }
    small.drain();
    for(int i = 0; i < 100; ++i)
    {
      if(channel->takeSnapshot() == SnapshotResult::ok)
      {
        ++successes;
      }
      small.drain();
      large.drain();
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
    auto sink = queueSink(1024, worker ? Delivery::Threaded : Delivery::Manual);
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
  auto sink = queueSink(1024, Delivery::Threaded);
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

TEST(SinkQueue, StopDuringPublicationDrainsEveryAcceptedSnapshot)
{
  auto sink = queueSink(1024, Delivery::Threaded);
  uint64_t value = 0;
  auto channel = channelWith(sink, &value);
  std::mutex mutex;
  std::condition_variable ready;
  bool entered = false, release = false;
  int stored = 0;
  sink->callback = [&](const SnapshotRef&) {
    std::unique_lock lock(mutex);
    ++stored;
    if(stored == 1)
    {
      entered = true;
      ready.notify_all();
      ready.wait(lock, [&] { return release; });
    }
  };
  ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
  {
    std::unique_lock lock(mutex);
    ASSERT_TRUE(ready.wait_for(lock, std::chrono::seconds(5), [&] { return entered; }));
  }
  ASSERT_EQ(channel->takeSnapshot(),
            SnapshotResult::ok);  // Guaranteed accepted work remains queued.
  std::atomic<bool> publish{ true }, attempted{ false };
  int accepted = 2;
  std::thread producer([&] {
    while(publish)
    {
      if(channel->takeSnapshot() == SnapshotResult::ok)
      {
        ++accepted;
      }
      attempted = true;
    }
  });
  while(!attempted)
  {
    std::this_thread::yield();
  }
  // stop() closes admission at once, then waits for the blocked callback.
  std::thread stopper([&] { sink.worker->stop(); });
  publish = false;
  producer.join();
  while(channel->takeSnapshot() ==
        SnapshotResult::ok)  // admitted until the close is observed
  {
    ++accepted;
  }
  {
    std::lock_guard lock(mutex);
    release = true;
  }
  ready.notify_all();
  stopper.join();
  EXPECT_EQ(stored, accepted);
  sink.worker->start();
  EXPECT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
  sink.drain();
  EXPECT_EQ(stored, accepted + 1);
  sink.worker->stop();
}

TEST(SinkQueue, RejectedWhenEverySinkRefusesPartialWhenSomeDo)
{
  uint64_t value = 1;
  auto stopped = queueSink(1024, Delivery::Threaded);
  auto full = queueSink(1);
  auto channel = channelWith(stopped, &value);
  ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
  stopped.worker->stop();  // admission closed: refuses without a full queue
  EXPECT_EQ(channel->takeSnapshot(), SnapshotResult::rejected);
  channel->addDataSink(full);
  // `full` accepts until its (block-rounded) queue fills, then both refuse.
  SnapshotResult result = SnapshotResult::partial;
  uint64_t attempts = 0;
  while(result == SnapshotResult::partial && attempts < 1000)
  {
    result = channel->takeSnapshot();
    ++attempts;
  }
  EXPECT_EQ(result, SnapshotResult::rejected);
  EXPECT_GT(attempts, 1u);
  EXPECT_EQ(channel->droppedSnapshots(stopped), 1 + attempts);
  EXPECT_EQ(channel->droppedSnapshots(full), 1u);
  stopped.worker->start();
  full.drain();
  EXPECT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
  stopped.worker->stop();
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
  auto sink = attach<MCAPSink>(Delivery::Threaded, 1, first, true);
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
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("data_tamer_rollover_" + std::to_string(NsecSinceEpoch().count()));
  std::filesystem::remove_all(directory);
  ASSERT_TRUE(std::filesystem::create_directory(directory));
  uint64_t value = 1;
  // Rolling over into new files is the default: no setCreateNewFileOnReset() (#98).
  auto sink = manual<MCAPSink>((directory / "rollover.tamer.mcap").string());
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

TEST(SinkQueue, McapTruncateOnResetIsOptIn)
{
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("data_tamer_truncate_" + std::to_string(NsecSinceEpoch().count()));
  std::filesystem::remove_all(directory);
  ASSERT_TRUE(std::filesystem::create_directory(directory));
  uint64_t value = 1;
  auto sink = manual<MCAPSink>((directory / "truncate.mcap").string());
  sink->setCreateNewFileOnReset(false);
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
    mcap::McapReader reader;
    ASSERT_TRUE(reader.open(file.path().string()).ok());
    size_t count = 0;
    for(const auto& message : reader.readMessages())
    {
      (void)message;
      ++count;
    }
    EXPECT_EQ(count, 0u);  // each reset discarded what came before
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

TEST(SinkQueue, McapRolloverRestartsSequencePerFile)
{
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("data_tamer_rollover_seq_" + std::to_string(NsecSinceEpoch().count()));
  std::filesystem::remove_all(directory);
  ASSERT_TRUE(std::filesystem::create_directory(directory));
  uint64_t value = 1;
  auto sink = manual<MCAPSink>((directory / "seq.mcap").string());
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
  auto first = queueSink(1024, Delivery::Threaded);
  auto second = queueSink(1024, Delivery::Threaded);
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
