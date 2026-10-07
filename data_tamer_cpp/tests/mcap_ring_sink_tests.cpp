// MCAPRingSink (#95): RAM ring of recent snapshots, written to MCAP on request.
#include "data_tamer/channel.hpp"
#include "data_tamer/data_tamer.hpp"
#include "data_tamer/sinks/mcap_ring_sink.hpp"
#include "data_tamer_parser/data_tamer_parser.hpp"
#include "alloc_counter.hpp"
#include "test_sinks.hpp"

#include <mcap/reader.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <future>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include <csignal>
#include <fstream>
#include <iterator>

#include <sys/resource.h>
#include <unistd.h>

using namespace DataTamer;
using std::chrono::nanoseconds;

namespace
{
struct Message
{
  std::string topic;
  int64_t timestamp;
  uint32_t sequence;
  std::map<std::string, double> fields;
};

std::vector<Message> readDump(const std::string& path)
{
  std::vector<Message> out;
  mcap::McapReader reader;
  EXPECT_TRUE(reader.open(path).ok()) << path;
  std::map<uint64_t, DataTamerParser::Schema> schemas;
  for(const auto& view : reader.readMessages())
  {
    auto it = schemas.find(view.schema->id);
    if(it == schemas.end())
    {
      const std::string text(reinterpret_cast<const char*>(view.schema->data.data()),
                             view.schema->data.size());
      it = schemas
               .emplace(view.schema->id, DataTamerParser::BuildSchemaFromText(text, true))
               .first;
    }
    DataTamerParser::BufferSpan body{ reinterpret_cast<const uint8_t*>(view.message.data),
                                      view.message.dataSize };
    DataTamerParser::SnapshotView snapshot{};
    snapshot.schema_hash = it->second.hash;
    snapshot.timestamp = view.message.logTime;
    const auto mask_size = DataTamerParser::Deserialize<uint32_t>(body);
    snapshot.active_mask = { body.data, mask_size };
    body.trimFront(mask_size);
    const auto payload_size = DataTamerParser::Deserialize<uint32_t>(body);
    snapshot.payload = { body.data, payload_size };

    Message& msg = out.emplace_back();
    msg.topic = view.channel->topic;
    msg.timestamp = int64_t(view.message.logTime);
    msg.sequence = view.message.sequence;
    EXPECT_TRUE(DataTamerParser::ParseSnapshot(
        it->second, snapshot,
        [&](const std::string& name, const DataTamerParser::VarNumber& number) {
          msg.fields[name] = std::visit([](auto v) { return double(v); }, number);
        }));
  }
  reader.close();
  return out;
}

std::vector<int64_t> timestamps(const std::vector<Message>& messages,
                                const std::string& topic = "")
{
  std::vector<int64_t> out;
  for(const auto& msg : messages)
  {
    if(topic.empty() || msg.topic == topic)
    {
      out.push_back(msg.timestamp);
    }
  }
  return out;
}

std::vector<int64_t> range(int64_t first, int64_t last, int64_t step)
{
  std::vector<int64_t> out;
  for(int64_t t = first; t <= last; t += step)
  {
    out.push_back(t);
  }
  return out;
}

// A unique base path per test; removes every dump it may have produced.
struct TempBase
{
  std::string path;
  explicit TempBase(const std::string& tag)
    : path((std::filesystem::temp_directory_path() /
            ("data_tamer_ring_" + tag + "_" + std::to_string(::getpid()) + ".mcap"))
               .string())
  {}
  std::string dump(size_t n) const { return details::NumberedPath(path, n); }
  ~TempBase()
  {
    for(size_t n = 1; n <= 10; ++n)
    {
      std::filesystem::remove(dump(n));
    }
  }
};

MCAPRingOptions options(const std::string& path, int64_t window_ns,
                        size_t capacity = 1 << 20)
{
  MCAPRingOptions opt;
  opt.filepath = path;
  opt.window = nanoseconds(window_ns);
  opt.capacity_bytes = capacity;
  return opt;
}

// One channel with a counter "value" equal to the timestamp.
struct Source
{
  std::shared_ptr<LogChannel> channel;
  int64_t value = 0;

  explicit Source(const std::string& name, const std::shared_ptr<SinkWorker>& sink)
    : channel(LogChannel::create(name))
  {
    channel->registerValue("value", &value);
    channel->addDataSink(sink);
    channel->prepare();
  }
  void take(int64_t ts)
  {
    value = ts;
    ASSERT_EQ(channel->tryTakeSnapshot(nanoseconds(ts)), SnapshotResult::ok);
  }
};
}  // namespace

TEST(MCAPRingSink, EvictsByAge)
{
  TempBase base("age");
  auto sink = DataTamerTest::manual<MCAPRingSink>(options(base.path, 100));
  Source source("age", sink);
  for(int64_t ts = 0; ts <= 1000; ts += 10)
  {
    source.take(ts);
    sink.drain();
  }
  // Kept: timestamps in [1000 - 100, 1000].
  EXPECT_EQ(sink->stats().stored_snapshots, 11u);
  EXPECT_EQ(sink->stats().evicted_by_capacity, 0u);
}

TEST(MCAPRingSink, EvictsByCapacity)
{
  TempBase base("capacity");
  size_t record = 0;
  {
    auto probe = DataTamerTest::manual<MCAPRingSink>(options(base.path, 1'000'000));
    Source source("capacity", probe);
    source.take(0);
    probe.drain();
    record = probe->stats().stored_bytes;
  }
  ASSERT_GT(record, 0u);
  // Room for 5 records and a bit, so records wrap around the buffer end.
  auto sink = DataTamerTest::manual<MCAPRingSink>(
      options(base.path, 1'000'000, 5 * record + record / 2));
  Source source("capacity", sink);
  for(int64_t ts = 0; ts < 20; ++ts)
  {
    source.take(ts);
    sink.drain();
  }
  auto stats = sink->stats();
  EXPECT_EQ(stats.stored_snapshots, 5u);
  EXPECT_EQ(stats.stored_bytes, 5 * record);
  EXPECT_EQ(stats.evicted_by_capacity, 15u);

  // The wrapped records read back intact. The ring could not hold the whole
  // window: the dump reports it as truncated.
  MCAPRingDump info;
  sink->setDumpCallback([&](const MCAPRingDump& dump) { info = dump; });
  ASSERT_TRUE(sink->requestDump());
  source.take(20);  // trigger
  source.take(21);  // completes [20 - window, 20]
  sink.drain();
  sink->waitForWriter();
  EXPECT_TRUE(info.ok) << info.error;
  EXPECT_TRUE(info.truncated);
  EXPECT_EQ(info.start, nanoseconds(20 - 1'000'000));
  EXPECT_EQ(info.first_message, nanoseconds(17));
  EXPECT_EQ(info.last_message, nanoseconds(20));
  const auto messages = readDump(base.dump(1));
  EXPECT_EQ(timestamps(messages), range(17, 20, 1));
  for(const auto& msg : messages)
  {
    EXPECT_EQ(msg.fields.at("value"), double(msg.timestamp));
  }

  // A snapshot larger than the whole ring is dropped, not stored.
  auto tiny = DataTamerTest::manual<MCAPRingSink>(options(base.path, 1000, record - 1));
  Source small("tiny", tiny);
  small.take(0);
  tiny.drain();
  EXPECT_EQ(tiny->stats().dropped_oversize, 1u);
  EXPECT_EQ(tiny->stats().stored_snapshots, 0u);
}

// stats() reports the timestamps of the first and the last stored record.
TEST(MCAPRingSink, StatsReportTheStoredInterval)
{
  TempBase base("interval");
  auto sink = DataTamerTest::manual<MCAPRingSink>(options(base.path, 100));
  auto stats = sink->stats();
  EXPECT_EQ(stats.stored_snapshots, 0u);
  EXPECT_EQ(stats.oldest_timestamp, nanoseconds(0)) << "empty ring";
  EXPECT_EQ(stats.newest_timestamp, nanoseconds(0)) << "empty ring";

  Source source("interval", sink);
  source.take(1000);
  sink.drain();
  stats = sink->stats();
  EXPECT_EQ(stats.oldest_timestamp, nanoseconds(1000));
  EXPECT_EQ(stats.newest_timestamp, nanoseconds(1000));

  for(int64_t ts = 1010; ts <= 1500; ts += 10)
  {
    source.take(ts);
    sink.drain();
  }
  stats = sink->stats();  // age eviction keeps [1500 - 100, 1500]
  EXPECT_EQ(stats.oldest_timestamp, nanoseconds(1400));
  EXPECT_EQ(stats.newest_timestamp, nanoseconds(1500));
  // stats() is composed from one getter per field.
  EXPECT_EQ(sink->oldestTimestamp(), nanoseconds(1400));
  EXPECT_EQ(sink->newestTimestamp(), nanoseconds(1500));
  EXPECT_EQ(sink->storedSnapshots(), stats.stored_snapshots);
  EXPECT_EQ(sink->storedBytes(), stats.stored_bytes);
  EXPECT_EQ(sink->evictedByCapacity(), 0u);
  EXPECT_EQ(sink->droppedOversize(), 0u);
  EXPECT_EQ(sink->writerBusyRetries(), 0u);
  EXPECT_EQ(sink->dumpsWritten(), 0u);
  EXPECT_EQ(sink->dumpsFailed(), 0u);

  // Capacity eviction: room for 3 records, so the interval is shorter than
  // the window.
  const size_t record = stats.stored_bytes / stats.stored_snapshots;
  auto small = DataTamerTest::manual<MCAPRingSink>(
      options(base.path, 1'000'000, 3 * record + record / 2));
  Source capped("capped", small);
  for(int64_t ts = 0; ts < 10; ++ts)
  {
    capped.take(ts);
    small.drain();
  }
  stats = small->stats();
  EXPECT_EQ(stats.stored_snapshots, 3u);
  EXPECT_EQ(stats.evicted_by_capacity, 7u);
  EXPECT_EQ(stats.oldest_timestamp, nanoseconds(7));
  EXPECT_EQ(stats.newest_timestamp, nanoseconds(9));

  // Delivery order, not timestamp order: an older snapshot of another channel
  // delivered last is the newest record.
  Source late("late", sink);
  late.take(1450);
  sink.drain();
  stats = sink->stats();
  EXPECT_EQ(stats.oldest_timestamp, nanoseconds(1400));
  EXPECT_EQ(stats.newest_timestamp, nanoseconds(1450));
}

// The dump holds [T - window, T + post] around the first snapshot after the
// request, in snapshot time, and completes at the first snapshot > T + post.
TEST(MCAPRingSink, DumpCoversPreAndPostWindow)
{
  TempBase base("window");
  auto sink = DataTamerTest::manual<MCAPRingSink>(options(base.path, 100));
  std::vector<MCAPRingDump> dumps;
  sink->setDumpCallback([&](const MCAPRingDump& dump) { dumps.push_back(dump); });
  Source source("window", sink);

  for(int64_t ts = 0; ts <= 150; ts += 10)
  {
    source.take(ts);
  }
  sink.drain();
  ASSERT_TRUE(sink->requestDump(nanoseconds(50)));
  EXPECT_TRUE(sink->dumpRequested());
  for(int64_t ts = 160; ts <= 210; ts += 10)
  {
    source.take(ts);
    sink.drain();
    EXPECT_TRUE(sink->dumpRequested()) << "post-trigger interval not over at " << ts;
  }
  source.take(220);  // past T + post: completes the dump
  sink.drain();
  EXPECT_FALSE(sink->dumpRequested());
  for(int64_t ts = 230; ts <= 300; ts += 10)
  {
    source.take(ts);
  }
  sink.drain();
  sink->waitForWriter();

  ASSERT_EQ(dumps.size(), 1u);
  EXPECT_TRUE(dumps[0].ok) << dumps[0].error;
  EXPECT_EQ(dumps[0].path, base.dump(1));
  EXPECT_EQ(dumps[0].trigger_time, nanoseconds(160));
  EXPECT_EQ(dumps[0].start, nanoseconds(60));
  EXPECT_EQ(dumps[0].end, nanoseconds(210));
  EXPECT_EQ(dumps[0].messages, 16u);
  EXPECT_EQ(dumps[0].first_message, nanoseconds(60));
  EXPECT_EQ(dumps[0].last_message, nanoseconds(210));
  EXPECT_FALSE(dumps[0].truncated);
  EXPECT_EQ(sink->stats().dumps_written, 1u);

  const auto messages = readDump(base.dump(1));
  EXPECT_EQ(timestamps(messages), range(60, 210, 10));
  for(size_t i = 0; i < messages.size(); ++i)
  {
    EXPECT_EQ(messages[i].topic, "window");
    EXPECT_EQ(messages[i].sequence, i + 1);
    EXPECT_EQ(messages[i].fields.at("value"), double(messages[i].timestamp));
  }
}

TEST(MCAPRingSink, DumpContainsEveryChannel)
{
  TempBase base("channels");
  auto sink = DataTamerTest::manual<MCAPRingSink>(options(base.path, 40));
  Source fast("fast", sink);
  Source slow("slow", sink);
  Source idle("idle", sink);  // schema announced, no snapshot in the window
  idle.take(0);
  for(int64_t ts = 100; ts <= 200; ts += 10)
  {
    fast.take(ts);
    if(ts % 20 == 0)
    {
      slow.take(ts);
    }
  }
  sink.drain();
  ASSERT_TRUE(sink->requestDump());
  fast.take(210);  // trigger
  fast.take(220);  // completes
  sink.drain();
  sink->waitForWriter();

  const auto messages = readDump(base.dump(1));
  EXPECT_EQ(timestamps(messages, "fast"), range(170, 210, 10));
  EXPECT_EQ(timestamps(messages, "slow"), range(180, 200, 20));
  EXPECT_TRUE(timestamps(messages, "idle").empty());
  std::map<std::string, std::vector<uint32_t>> sequences;
  for(const auto& msg : messages)
  {
    sequences[msg.topic].push_back(msg.sequence);
  }
  EXPECT_EQ(sequences["fast"], (std::vector<uint32_t>{ 1, 2, 3, 4, 5 }));
  EXPECT_EQ(sequences["slow"], (std::vector<uint32_t>{ 1, 2 }));

  mcap::McapReader reader;
  ASSERT_TRUE(reader.open(base.dump(1)).ok());
  ASSERT_TRUE(reader.readSummary(mcap::ReadSummaryMethod::NoFallbackScan).ok());
  EXPECT_EQ(reader.channels().size(), 2u);
  EXPECT_EQ(reader.schemas().size(), 2u);
}

// One active request at a time: further requests are ignored until the dump is
// handed to the writer, then accepted again.
TEST(MCAPRingSink, RequestsWhileActiveAreIgnored)
{
  TempBase base("ignored");
  auto sink = DataTamerTest::manual<MCAPRingSink>(options(base.path, 1000));
  Source source("ignored", sink);
  source.take(0);
  sink.drain();

  ASSERT_TRUE(sink->requestDump(nanoseconds(20)));
  EXPECT_FALSE(sink->requestDump(nanoseconds(1000))) << "pending, not triggered";
  source.take(10);  // trigger: end = 30
  sink.drain();
  EXPECT_FALSE(sink->requestDump()) << "collecting the post-trigger interval";
  source.take(30);
  sink.drain();
  EXPECT_FALSE(sink->requestDump()) << "at T + post, not complete yet";
  source.take(31);
  sink.drain();
  ASSERT_FALSE(sink->dumpRequested());
  EXPECT_TRUE(sink->requestDump()) << "handed to the writer: accepted again";
  sink->waitForWriter();  // or dump 2 would wait for a third snapshot
  source.take(40);
  source.take(41);
  sink.drain();
  sink->waitForWriter();

  EXPECT_EQ(sink->stats().dumps_written, 2u);
  EXPECT_EQ(timestamps(readDump(base.dump(1))), (std::vector<int64_t>{ 0, 10, 30 }));
  EXPECT_EQ(timestamps(readDump(base.dump(2))),
            (std::vector<int64_t>{ 0, 10, 30, 31, 40 }));
}

// A finished dump that finds the writer busy waits for a later snapshot; the
// ring keeps its interval meanwhile and the worker never blocks.
TEST(MCAPRingSink, WriterBusyRetriesOnNextSnapshot)
{
  TempBase base("busy");
  auto sink = DataTamerTest::manual<MCAPRingSink>(options(base.path, 30));
  std::promise<void> release;
  auto released = release.get_future().share();
  std::atomic<int> callbacks{ 0 };
  sink->setDumpCallback([&](const MCAPRingDump&) {
    if(callbacks.fetch_add(1) == 0)
    {
      released.wait();  // keep the writer busy with dump 1
    }
  });
  Source source("busy", sink);
  for(int64_t ts = 0; ts <= 50; ts += 10)
  {
    source.take(ts);
  }
  sink.drain();
  ASSERT_TRUE(sink->requestDump());
  source.take(60);  // dump 1: [30, 60]
  source.take(65);
  sink.drain();

  ASSERT_TRUE(sink->requestDump());
  source.take(70);  // dump 2: [40, 70]
  source.take(75);  // complete, writer busy
  sink.drain();
  EXPECT_EQ(sink->stats().writer_busy_retries, 1u);
  EXPECT_TRUE(sink->dumpRequested());
  for(int64_t ts = 80; ts <= 150; ts += 10)  // the ring must keep [40, 70]
  {
    source.take(ts);
    sink.drain();
  }
  EXPECT_EQ(sink->stats().writer_busy_retries, 1u) << "counted once per dump";

  release.set_value();
  while(callbacks.load() == 0 || sink->stats().dumps_written == 0)
  {
    std::this_thread::yield();
  }
  sink->waitForWriter();
  source.take(160);  // hand-off succeeds
  sink.drain();
  EXPECT_FALSE(sink->dumpRequested());
  sink->waitForWriter();
  EXPECT_EQ(sink->stats().dumps_written, 2u);
  EXPECT_EQ(timestamps(readDump(base.dump(1))), range(30, 60, 10));
  EXPECT_EQ(timestamps(readDump(base.dump(2))),
            (std::vector<int64_t>{ 40, 50, 60, 65, 70 }));
}

// A complete dump waiting for a busy writer can lose its whole interval to
// capacity eviction; it must still report truncated, even though the newest
// evicted timestamp is past its end.
TEST(MCAPRingSink, DumpWaitingForTheWriterReportsLaterEvictions)
{
  TempBase base("busy_evicted");
  size_t record = 0;
  {
    auto probe = DataTamerTest::manual<MCAPRingSink>(options(base.path, 1000));
    Source source("busy_evicted", probe);
    source.take(0);
    probe.drain();
    record = probe->storedBytes();
  }
  ASSERT_GT(record, 0u);
  auto sink = DataTamerTest::manual<MCAPRingSink>(
      options(base.path, 30, 7 * record + record / 2));  // room for 7 records
  std::promise<void> release;
  auto released = release.get_future().share();
  std::mutex dumps_mutex;
  std::vector<MCAPRingDump> dumps;
  sink->setDumpCallback([&](const MCAPRingDump& dump) {
    size_t count = 0;
    {
      std::scoped_lock lock(dumps_mutex);
      dumps.push_back(dump);
      count = dumps.size();
    }
    if(count == 1)
    {
      released.wait();  // keep the writer busy with dump 1
    }
  });
  Source source("busy_evicted", sink);
  for(int64_t ts = 0; ts <= 40; ts += 10)
  {
    source.take(ts);
  }
  sink.drain();
  ASSERT_TRUE(sink->requestDump());
  source.take(50);  // dump 1: [20, 50]
  source.take(51);
  sink.drain();
  ASSERT_TRUE(sink->requestDump());
  source.take(60);  // dump 2: [30, 60]
  source.take(61);  // complete, writer busy
  sink.drain();
  ASSERT_EQ(sink->writerBusyRetries(), 1u);
  // Evicts all of [30, 60] and then 61: the newest evicted timestamp is past
  // the end of dump 2.
  for(int64_t ts = 70; ts <= 130; ts += 10)
  {
    source.take(ts);
    sink.drain();
  }
  EXPECT_EQ(sink->oldestTimestamp(), nanoseconds(70));

  release.set_value();
  while(sink->dumpsWritten() == 0)
  {
    std::this_thread::yield();
  }
  sink->waitForWriter();
  source.take(140);  // hand-off of dump 2
  sink.drain();
  sink->waitForWriter();

  std::scoped_lock lock(dumps_mutex);
  ASSERT_EQ(dumps.size(), 2u);
  EXPECT_FALSE(dumps[0].truncated);
  EXPECT_EQ(dumps[1].start, nanoseconds(30));
  EXPECT_EQ(dumps[1].end, nanoseconds(60));
  EXPECT_EQ(dumps[1].messages, 0u);
  EXPECT_TRUE(dumps[1].truncated);
}

// A request made just before shutdown, with no snapshot left to trigger it:
// SinkWorker::stop() writes it (DataSink::onStop()).
TEST(MCAPRingSink, StopWritesThePendingDump)
{
  TempBase base("flush");
  auto worker = MCAPRingSink::create(options(base.path, 100));
  auto& sink = worker->as<MCAPRingSink>();
  Source source("flush", worker);
  EXPECT_FALSE(sink.flushPendingDump()) << "nothing requested";
  for(int64_t ts = 0; ts <= 300; ts += 10)
  {
    source.take(ts);
  }
  worker->drain();  // every snapshot delivered before the request
  ASSERT_TRUE(sink.requestDump(nanoseconds(1000)));
  EXPECT_TRUE(sink.dumpRequested());
  worker->stop();  // no snapshot left to trigger the request
  EXPECT_FALSE(sink.dumpRequested());
  EXPECT_EQ(sink.stats().dumps_written, 1u);  // on disk when stop() returns
  // The latest snapshot is the trigger.
  EXPECT_EQ(timestamps(readDump(base.dump(1))), range(200, 300, 10));
  EXPECT_FALSE(sink.flushPendingDump());
  worker->stop();  // idempotent: no second dump
  EXPECT_EQ(sink.stats().dumps_written, 1u);
  EXPECT_EQ(worker->errors(), 0u);
}

// The snapshots still queued at stop() are delivered before the dump is cut,
// so they complete it instead of being cut off.
TEST(MCAPRingSink, StopDeliversQueuedSnapshotsBeforeWritingTheDump)
{
  TempBase base("stop_queued");
  auto sink = DataTamerTest::manual<MCAPRingSink>(options(base.path, 100));
  MCAPRingDump info;
  sink->setDumpCallback([&](const MCAPRingDump& dump) { info = dump; });
  Source source("stop_queued", sink);
  for(int64_t ts = 0; ts <= 50; ts += 10)
  {
    source.take(ts);
  }
  sink.drain();
  ASSERT_TRUE(sink->requestDump(nanoseconds(1000)));
  source.take(60);  // queued, not delivered: the trigger
  source.take(70);
  sink.worker->stop();
  EXPECT_FALSE(sink->dumpRequested());
  EXPECT_TRUE(info.ok);
  EXPECT_EQ(info.trigger_time, nanoseconds(60));
  EXPECT_EQ(info.end, nanoseconds(70));
  EXPECT_EQ(timestamps(readDump(base.dump(1))), range(0, 70, 10));
}

TEST(MCAPRingSink, FlushPendingDumpCutsThePostTriggerInterval)
{
  TempBase base("flush_cut");
  auto sink = DataTamerTest::manual<MCAPRingSink>(options(base.path, 20));
  MCAPRingDump info;
  sink->setDumpCallback([&](const MCAPRingDump& dump) { info = dump; });
  Source source("flush_cut", sink);
  for(int64_t ts = 0; ts <= 50; ts += 10)
  {
    source.take(ts);
  }
  sink.drain();
  ASSERT_TRUE(sink->requestDump(nanoseconds(1000)));
  source.take(60);
  source.take(70);
  sink.drain();
  ASSERT_TRUE(sink->dumpRequested());
  ASSERT_TRUE(sink->flushPendingDump());
  EXPECT_EQ(info.trigger_time, nanoseconds(60));
  EXPECT_EQ(info.end, nanoseconds(70));
  EXPECT_EQ(timestamps(readDump(base.dump(1))), range(40, 70, 10));
}

// After warm-up, onSnapshot allocates nothing on the worker thread, including
// the trigger and the hand-off of a dump to the writer.
TEST(MCAPRingSink, OnSnapshotDoesNotAllocate)
{
  TempBase base("alloc");
  auto sink =
      DataTamerTest::manual<MCAPRingSink>(options(base.path, 1'000'000'000, 4096));
  Source source("alloc", sink);
  int64_t ts = 0;
  auto cycle = [&] {
    EXPECT_TRUE(sink->requestDump(nanoseconds(20)));
    for(int i = 0; i < 200; ++i, ts += 10)  // wraps the 4 KiB ring many times
    {
      source.take(ts);
      sink.drain();
    }
  };
  cycle();  // warm-up: the channel and queue settle
  sink->waitForWriter();
  size_t allocations = 0;
  {
    DataTamerTest::AllocCounter::Scope scope;
    cycle();
    allocations = scope.allocations();
  }
  sink->waitForWriter();
  EXPECT_EQ(allocations, 0u);
  EXPECT_EQ(sink->stats().dumps_written, 2u);
  EXPECT_GT(sink->stats().evicted_by_capacity, 0u);
}

// requestDump() from another thread while the worker thread stores snapshots.
TEST(MCAPRingSink, RequestFromAnotherThread)
{
  TempBase base("threads");
  auto worker = MCAPRingSink::create(options(base.path, 1000));
  auto& sink = worker->as<MCAPRingSink>();
  std::atomic<int> callbacks{ 0 };
  std::atomic<bool> all_ok{ true };
  sink.setDumpCallback([&](const MCAPRingDump& dump) {
    all_ok = all_ok && dump.ok;
    ++callbacks;
  });
  Source source("threads", worker);

  std::atomic<bool> done{ false };
  std::thread producer([&] {
    for(int64_t ts = 0; ts < 3000; ++ts)
    {
      source.value = ts;
      while(source.channel->tryTakeSnapshot(nanoseconds(ts)) != SnapshotResult::ok)
      {
        std::this_thread::yield();
      }
    }
    done = true;
  });
  int accepted = 0;
  while(!done)
  {
    if(sink.requestDump(nanoseconds(5)))
    {
      ++accepted;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  producer.join();
  worker->stop();  // writes a request accepted near the end
  EXPECT_FALSE(sink.dumpRequested());
  EXPECT_GE(accepted, 1);
  EXPECT_EQ(callbacks.load(), accepted);
  EXPECT_TRUE(all_ok.load());
  EXPECT_EQ(sink.stats().dumps_written, uint64_t(accepted));
  EXPECT_EQ(worker->errors(), 0u);
  for(int n = 1; n <= accepted; ++n)  // TempBase removes 10 at most
  {
    std::filesystem::remove(base.dump(size_t(n)));
  }
}

// Regression: flushPendingDump() must not hold the ring lock while it waits for
// a busy writer, whose callback may call stats(). Waiting for the writer from
// its own callback throws instead of deadlocking.
TEST(MCAPRingSink, CallbackMayQueryWhileFlushWaits)
{
  TempBase base("callback_flush");
  auto sink = DataTamerTest::manual<MCAPRingSink>(options(base.path, 100));
  std::promise<void> release;
  auto released = release.get_future().share();
  std::atomic<int> callbacks{ 0 };
  std::atomic<int> logic_errors{ 0 };
  MCAPRingSink* recorder = sink.sink;
  sink->setDumpCallback([&, recorder](const MCAPRingDump&) {
    if(callbacks.fetch_add(1) == 0)
    {
      released.wait();
    }
    (void)recorder->stats();
    try
    {
      recorder->waitForWriter();
    }
    catch(const std::logic_error&)
    {
      ++logic_errors;
    }
    try
    {
      (void)recorder->flushPendingDump();
    }
    catch(const std::logic_error&)
    {
      ++logic_errors;
    }
  });
  Source source("callback_flush", sink);
  source.take(0);
  sink.drain();
  ASSERT_TRUE(sink->requestDump());
  source.take(10);
  source.take(11);  // dump 1 handed off: the writer blocks in the callback
  sink.drain();
  ASSERT_TRUE(sink->requestDump());
  source.take(20);
  source.take(21);  // dump 2 complete, pending behind the busy writer
  sink.drain();
  ASSERT_TRUE(sink->dumpRequested());

  auto flushed =
      std::async(std::launch::async, [recorder] { return recorder->flushPendingDump(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));  // let it wait
  release.set_value();
  if(flushed.wait_for(std::chrono::seconds(20)) != std::future_status::ready)
  {
    ADD_FAILURE() << "flushPendingDump() deadlocked with the dump callback";
    std::abort();  // fail instead of hanging
  }
  EXPECT_TRUE(flushed.get());
  EXPECT_EQ(callbacks.load(), 2);
  EXPECT_EQ(logic_errors.load(), 4);
  EXPECT_EQ(sink->stats().dumps_written, 2u);
}

// Channels queue separately, so another channel's snapshot stamped at T + post
// may arrive after one stamped there: the dump waits for a later timestamp.
TEST(MCAPRingSink, SnapshotsAtTheEndFromAnotherChannelAreIncluded)
{
  TempBase base("same_time");
  auto sink = DataTamerTest::manual<MCAPRingSink>(options(base.path, 100));
  Source a("a", sink);
  Source b("b", sink);
  ASSERT_TRUE(sink->requestDump());
  a.take(100);  // trigger, T = end = 100
  sink.drain();
  b.take(100);
  sink.drain();
  EXPECT_TRUE(sink->dumpRequested());
  a.take(110);  // completes
  sink.drain();
  sink->waitForWriter();
  const auto messages = readDump(base.dump(1));
  EXPECT_EQ(timestamps(messages, "a"), (std::vector<int64_t>{ 100 }));
  EXPECT_EQ(timestamps(messages, "b"), (std::vector<int64_t>{ 100 }));
}

// The flush uses the newest timestamp seen, not the last one delivered.
TEST(MCAPRingSink, FlushUsesTheNewestTimestamp)
{
  TempBase base("newest");
  auto sink = DataTamerTest::manual<MCAPRingSink>(options(base.path, 50));
  MCAPRingDump info;
  sink->setDumpCallback([&](const MCAPRingDump& dump) { info = dump; });
  Source a("a", sink);
  Source b("b", sink);
  a.take(300);
  b.take(100);  // delivered last, older
  sink.drain();
  ASSERT_TRUE(sink->requestDump());
  ASSERT_TRUE(sink->flushPendingDump());
  EXPECT_TRUE(info.ok) << info.error;
  EXPECT_EQ(info.trigger_time, nanoseconds(300));
  EXPECT_EQ(info.start, nanoseconds(250));
  EXPECT_EQ(info.end, nanoseconds(300));
  EXPECT_EQ(timestamps(readDump(base.dump(1))), (std::vector<int64_t>{ 300 }));
}

// flushPendingDump() with the worker running: no stop() and start() around it,
// and the worker keeps delivering and triggering dumps afterwards.
TEST(MCAPRingSink, FlushPendingDumpWhileWorkerRuns)
{
  TempBase base("flush_live");
  auto worker = MCAPRingSink::create(options(base.path, 100));
  auto& sink = worker->as<MCAPRingSink>();
  std::vector<MCAPRingDump> dumps;
  std::mutex dumps_mutex;
  sink.setDumpCallback([&](const MCAPRingDump& dump) {
    std::scoped_lock lock(dumps_mutex);
    dumps.push_back(dump);
  });
  Source source("flush_live", worker);
  for(int64_t ts = 0; ts <= 300; ts += 10)
  {
    source.take(ts);
  }
  worker->drain();

  // Requested, not triggered: the newest snapshot is the trigger.
  ASSERT_TRUE(sink.requestDump(nanoseconds(1000)));
  ASSERT_TRUE(sink.flushPendingDump());
  EXPECT_FALSE(sink.dumpRequested());

  // Triggered and collecting: cut at the newest snapshot.
  ASSERT_TRUE(sink.requestDump(nanoseconds(1000)));
  source.take(310);  // trigger
  source.take(320);
  worker->drain();
  ASSERT_TRUE(sink.dumpRequested());
  ASSERT_TRUE(sink.flushPendingDump());
  EXPECT_FALSE(sink.dumpRequested());

  // The worker still delivers: a snapshot completes the next dump.
  ASSERT_TRUE(sink.requestDump());
  source.take(330);  // trigger
  source.take(340);  // completes [230, 330]
  worker->drain();
  sink.waitForWriter();
  EXPECT_FALSE(sink.flushPendingDump()) << "nothing left to write";
  EXPECT_EQ(worker->errors(), 0u);

  std::scoped_lock lock(dumps_mutex);
  ASSERT_EQ(dumps.size(), 3u);
  EXPECT_EQ(dumps[0].trigger_time, nanoseconds(300));
  EXPECT_EQ(dumps[0].end, nanoseconds(300));
  EXPECT_EQ(dumps[1].trigger_time, nanoseconds(310));
  EXPECT_EQ(dumps[1].end, nanoseconds(320));
  EXPECT_EQ(dumps[2].trigger_time, nanoseconds(330));
  EXPECT_EQ(dumps[2].end, nanoseconds(330));
  EXPECT_EQ(timestamps(readDump(base.dump(1))), range(200, 300, 10));
  EXPECT_EQ(timestamps(readDump(base.dump(2))), range(210, 320, 10));
  EXPECT_EQ(timestamps(readDump(base.dump(3))), range(230, 330, 10));
}

namespace
{
// A threaded MCAPRingSink fed by a producer thread with increasing timestamps,
// and a dump callback that records every dump and keeps the writer busy for
// a varying time, so that flushes, hand-offs and requests interleave.
struct RaceFixture
{
  TempBase base;
  std::shared_ptr<SinkWorker> worker;
  MCAPRingSink& sink;
  std::mutex dumps_mutex;
  std::vector<MCAPRingDump> dumps;
  std::atomic<int> callbacks{ 0 };
  Source source;
  std::atomic<bool> done{ false };
  std::thread producer;

  explicit RaceFixture(const std::string& tag)
    : base(tag)
    , worker(MCAPRingSink::create(options(base.path, 50)))
    , sink(worker->as<MCAPRingSink>())
    , source(tag, worker)
  {
    sink.setDumpCallback([this](const MCAPRingDump& dump) {
      size_t count = 0;
      {
        std::scoped_lock lock(dumps_mutex);
        dumps.push_back(dump);
        count = dumps.size();
      }
      std::this_thread::sleep_for(std::chrono::microseconds(30 * (count % 4)));
      ++callbacks;
    });
    source.take(0);
    worker->drain();  // the ring holds a snapshot: every request is flushable
    producer = std::thread([this] {
      for(int64_t ts = 1; !done; ++ts)
      {
        source.value = ts;
        while(!done &&
              source.channel->tryTakeSnapshot(nanoseconds(ts)) != SnapshotResult::ok)
        {
          std::this_thread::yield();
        }
      }
    });
  }

  void finish()
  {
    done = true;
    producer.join();
    worker->stop();
  }

  // Every dump written, each interval consistent and non-empty.
  void check(uint64_t expected)
  {
    EXPECT_EQ(worker->errors(), 0u);
    EXPECT_EQ(sink.stats().dumps_written, expected);
    EXPECT_EQ(sink.stats().dumps_failed, 0u);
    std::scoped_lock lock(dumps_mutex);
    EXPECT_EQ(dumps.size(), expected);
    for(const auto& dump : dumps)
    {
      EXPECT_TRUE(dump.ok) << dump.error;
      EXPECT_LE(dump.start, dump.trigger_time);
      EXPECT_LE(dump.trigger_time, dump.end);
      // A dump that waited long for the writer may have lost its interval to
      // capacity eviction (the producer is not throttled); it must say so.
      if(dump.messages == 0)
      {
        EXPECT_TRUE(dump.truncated) << dump.path;
        continue;
      }
      EXPECT_GE(dump.first_message, dump.start);
      EXPECT_LE(dump.last_message, dump.end);
    }
  }

  ~RaceFixture()
  {
    if(producer.joinable())  // a failed ASSERT skipped finish()
    {
      done = true;
      producer.join();
    }
    worker->stop();
    sink.waitForWriter();                      // the callback uses this fixture
    for(size_t n = 1; n <= dumps.size(); ++n)  // TempBase removes 10 at most
    {
      std::filesystem::remove(base.dump(n));
    }
  }
};
}  // namespace

// flushPendingDump() racing onSnapshot(): each request is flushed at a varying
// point of its life (pending, collecting, complete and waiting for a busy
// writer, or already handed off by a snapshot). Every request is written once,
// and the flush returns only after it is. Run under ThreadSanitizer too.
TEST(MCAPRingSink, FlushRacesOnSnapshot)
{
  constexpr int kRounds = 300;
  RaceFixture race("flush_race");
  auto& sink = race.sink;
  // 0: completes at the second snapshot; 3: a few snapshots later;
  // huge: only the flush can finish it.
  const nanoseconds post_trigger[] = { nanoseconds(0), nanoseconds(3),
                                       nanoseconds(1'000'000'000) };
  int requests = 0;
  for(int round = 0; round < kRounds; ++round)
  {
    if(round % 2 == 1)
    {
      // A first request completed by the snapshots, so that the writer is
      // busy while the next one triggers, completes and retries its hand-off.
      ASSERT_TRUE(sink.requestDump()) << "round " << round;
      ++requests;
      while(sink.dumpRequested())
      {
        std::this_thread::yield();
      }
    }
    ASSERT_TRUE(sink.requestDump(post_trigger[round % 3])) << "round " << round;
    ++requests;
    for(int i = 0; i < (round * 7) % 50; ++i)  // vary where the flush lands
    {
      std::this_thread::yield();
    }
    (void)sink.flushPendingDump();
    ASSERT_FALSE(sink.dumpRequested()) << "round " << round;
    ASSERT_EQ(race.callbacks.load(), requests) << "returned before the dump was written";
  }
  race.finish();
  race.check(uint64_t(requests));
}

// The same with requestDump() called from another thread, as a real-time
// thread would, while flushPendingDump() runs: a flush may find the writer
// taken by a dump handed off after its waitForWriter(), or see its request
// handed off by a snapshot between two attempts.
TEST(MCAPRingSink, FlushRacesOnSnapshotAndRequests)
{
  RaceFixture race("flush_race_requests");
  auto& sink = race.sink;
  std::atomic<bool> stop_requests{ false };
  std::atomic<uint64_t> accepted{ 0 };
  std::thread requester([&] {
    for(int i = 0; !stop_requests; ++i)
    {
      if(sink.requestDump(nanoseconds(i % 3 == 0 ? 0 : 2)))
      {
        ++accepted;
      }
      std::this_thread::yield();
    }
  });
  int flushes = 0;
  while(accepted < 300 || flushes < 300)
  {
    (void)sink.flushPendingDump();
    ++flushes;
  }
  stop_requests = true;
  requester.join();
  race.finish();
  (void)sink.flushPendingDump();  // a request accepted near the end
  EXPECT_FALSE(sink.dumpRequested());
  race.check(accepted.load());
}

// Dumps never overwrite an existing file, e.g. of a previous run.
TEST(MCAPRingSink, SkipsExistingFileNames)
{
  TempBase base("existing");
  {
    std::ofstream(base.dump(1)) << "previous run";
  }
  auto sink = DataTamerTest::manual<MCAPRingSink>(options(base.path, 100));
  MCAPRingDump info;
  sink->setDumpCallback([&](const MCAPRingDump& dump) { info = dump; });
  Source source("existing", sink);
  source.take(0);
  sink.drain();
  ASSERT_TRUE(sink->requestDump());
  ASSERT_TRUE(sink->flushPendingDump());
  EXPECT_EQ(info.path, base.dump(2));
  EXPECT_EQ(timestamps(readDump(base.dump(2))), (std::vector<int64_t>{ 0 }));
  std::ifstream previous(base.dump(1));
  const std::string content((std::istreambuf_iterator<char>(previous)),
                            std::istreambuf_iterator<char>());
  EXPECT_EQ(content, "previous run");
}

TEST(MCAPRingSink, ReportsOpenFailure)
{
  auto sink = DataTamerTest::manual<MCAPRingSink>(
      options("/nonexistent_dir_data_tamer/dump.mcap", 100));
  MCAPRingDump info;
  sink->setDumpCallback([&](const MCAPRingDump& dump) { info = dump; });
  Source source("open_failure", sink);
  source.take(0);
  sink.drain();
  ASSERT_TRUE(sink->requestDump());
  ASSERT_TRUE(sink->flushPendingDump());
  EXPECT_FALSE(info.ok);
  EXPECT_FALSE(info.error.empty());
  EXPECT_EQ(sink->stats().dumps_failed, 1u);
  EXPECT_EQ(sink->stats().dumps_written, 0u);
}

// A real write failure: the file size limit makes write() fail with EFBIG.
TEST(MCAPRingSink, ReportsWriteFailure)
{
  TempBase base("write_failure");
  auto sink = DataTamerTest::manual<MCAPRingSink>(options(base.path, 1'000'000));
  MCAPRingDump info;
  sink->setDumpCallback([&](const MCAPRingDump& dump) { info = dump; });
  Source source("write_failure", sink);
  for(int64_t ts = 0; ts < 500; ++ts)  // about 20 KB of records
  {
    source.take(ts);
    sink.drain();
  }
  rlimit original{};
  ASSERT_EQ(::getrlimit(RLIMIT_FSIZE, &original), 0);
  if(original.rlim_max != RLIM_INFINITY && original.rlim_max < 4096)
  {
    GTEST_SKIP() << "file size limit already below the test size";
  }
  bool flushed = false;
  {
    // Restores the limit and the handler on every path, ASSERTs included, so
    // that later tests in this process can write files.
    struct Restore
    {
      rlimit limit;
      void (*handler)(int);
      ~Restore()
      {
        ::setrlimit(RLIMIT_FSIZE, &limit);
        std::signal(SIGXFSZ, handler);
      }
    } restore{ original, std::signal(SIGXFSZ, SIG_IGN) };
    rlimit limited = original;
    limited.rlim_cur = 4096;
    ASSERT_EQ(::setrlimit(RLIMIT_FSIZE, &limited), 0);
    ASSERT_TRUE(sink->requestDump());
    flushed = sink->flushPendingDump();
  }

  ASSERT_TRUE(flushed);
  EXPECT_FALSE(info.ok);
  EXPECT_NE(info.error.find("write failed"), std::string::npos) << info.error;
  EXPECT_EQ(info.path, base.dump(1));
  EXPECT_EQ(sink->stats().dumps_failed, 1u);
}
