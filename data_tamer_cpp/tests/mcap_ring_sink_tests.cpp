// MCAPRingSink (#95): RAM ring of recent snapshots, written to MCAP on request.
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

// A request made just before shutdown, with no snapshot left to trigger it.
TEST(MCAPRingSink, FlushPendingDumpAfterStop)
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
  worker->stop();  // no snapshot left to trigger the request
  EXPECT_TRUE(sink.dumpRequested());
  ASSERT_TRUE(sink.flushPendingDump());
  EXPECT_FALSE(sink.dumpRequested());
  EXPECT_EQ(sink.stats().dumps_written, 1u);
  // The latest snapshot is the trigger.
  EXPECT_EQ(timestamps(readDump(base.dump(1))), range(200, 300, 10));
  EXPECT_FALSE(sink.flushPendingDump());
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
  worker->stop();
  (void)sink.flushPendingDump();  // a request accepted near the end
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
