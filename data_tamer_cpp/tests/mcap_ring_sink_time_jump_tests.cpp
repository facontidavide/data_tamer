// MCAPRingSink after the snapshot clock jumped back, as in a simulation reset.
#include "data_tamer/channel.hpp"
#include "data_tamer/sinks/mcap_ring_sink.hpp"
#include "data_tamer/sinks/mcap_sink.hpp"
#include "mcap_test_utils.hpp"
#include "test_sinks.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <vector>

using namespace DataTamer;
using DataTamerTest::channelWith;
using DataTamerTest::logTimes;
using DataTamerTest::manual;
using DataTamerTest::ScratchDir;
using std::chrono::seconds;

// A request that no snapshot has triggered yet is flushed around the newest timestamp of
// the current run. When the clock of a channel steps back a new run starts: the maximum
// timestamp ever seen belongs to the old run, and the dump would hold only its snapshots.
TEST(MCAPRingSinkTimeJump, FlushOfAnUntriggeredRequestStaysOnTheNewRun)
{
  for(const bool by_stop : { false, true })
  {
    ScratchDir dir("ring_time_jump");
    MCAPRingOptions options;
    options.filepath = dir.file("jump.mcap");
    options.window = seconds(10);
    options.capacity_bytes = 1 << 20;
    auto sink = manual<MCAPRingSink>(options);
    int64_t value = 0;
    auto channel = channelWith(sink, &value);
    for(const int64_t at : { 995, 996, 997, 998, 999, 1000, 1, 2, 3, 4, 5 })
    {
      ASSERT_EQ(channel->takeSnapshot(seconds(at)), SnapshotResult::ok);
    }
    sink.drain();

    ASSERT_TRUE(sink->requestDump());
    if(by_stop)
    {
      sink.worker->stop();
    }
    else
    {
      EXPECT_TRUE(sink->flushPendingDump());
    }

    EXPECT_EQ(logTimes(details::NumberedPath(options.filepath, 1)),
              (std::vector<uint64_t>{ 1'000'000'000, 2'000'000'000, 3'000'000'000,
                                      4'000'000'000, 5'000'000'000 }))
        << (by_stop ? "flushed by stop()" : "flushed by flushPendingDump()");
  }
}

// Several channels jumping back: the newest timestamp of the new run is used, not the
// last one delivered, and nothing from before the jump is in the dump.
TEST(MCAPRingSinkTimeJump, FlushAfterAJumpUsesTheNewestTimestampOfTheNewRun)
{
  ScratchDir dir("ring_time_jump_channels");
  MCAPRingOptions options;
  options.filepath = dir.file("jump.mcap");
  options.window = seconds(3);
  options.capacity_bytes = 1 << 20;
  auto sink = manual<MCAPRingSink>(options);
  int64_t value = 0;
  auto a = channelWith(sink, &value, "a");
  auto b = channelWith(sink, &value, "b");
  const auto take = [](const std::shared_ptr<LogChannel>& channel, int64_t at) {
    ASSERT_EQ(channel->takeSnapshot(seconds(at)), SnapshotResult::ok);
  };
  take(a, 1000);
  take(b, 999);
  take(a, 1);  // both clocks restart, and b lags behind a
  take(b, 2);
  take(a, 6);
  take(b, 5);
  sink.drain();

  ASSERT_TRUE(sink->requestDump());
  ASSERT_TRUE(sink->flushPendingDump());

  // The newest timestamp is 6 s: the dump is [3 s, 6 s], a at 6 s and b at 5 s.
  EXPECT_EQ(logTimes(details::NumberedPath(options.filepath, 1)),
            (std::vector<uint64_t>{ 6'000'000'000, 5'000'000'000 }));
}

// A dump triggered before the jump is still collecting its post-trigger interval when the
// clock steps back. It is cut at the newest timestamp ever seen, so it keeps the run of
// its trigger instead of ending before it starts.
TEST(MCAPRingSinkTimeJump, FlushOfACollectingDumpKeepsTheRunOfItsTrigger)
{
  ScratchDir dir("ring_time_jump_collecting");
  MCAPRingOptions options;
  options.filepath = dir.file("jump.mcap");
  options.window = seconds(10);
  options.capacity_bytes = 1 << 20;
  auto sink = manual<MCAPRingSink>(options);
  int64_t value = 0;
  auto channel = channelWith(sink, &value);
  const auto take = [&](int64_t at) {
    ASSERT_EQ(channel->takeSnapshot(seconds(at)), SnapshotResult::ok);
  };
  take(1000);
  sink.drain();
  ASSERT_TRUE(sink->requestDump(seconds(100)));
  take(1001);  // the trigger: the dump collects until 1101 s
  take(1);     // the clock steps back
  take(2);
  sink.drain();
  ASSERT_TRUE(sink->dumpRequested());

  ASSERT_TRUE(sink->flushPendingDump());

  // [991 s, 1001 s]: what the dump had when the clock stepped back
  EXPECT_EQ(logTimes(details::NumberedPath(options.filepath, 1)),
            (std::vector<uint64_t>{ 1'000'000'000'000, 1'001'000'000'000 }));
}
