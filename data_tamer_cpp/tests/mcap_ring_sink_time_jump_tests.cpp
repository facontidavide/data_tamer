// MCAPRingSink after the snapshot clock jumped back, as in a simulation reset, or
// stepped back a little, as with snapshots of several threads.
#include "data_tamer/channel.hpp"
#include "data_tamer/sinks/mcap_ring_sink.hpp"
#include "data_tamer/sinks/mcap_sink.hpp"
#include "mcap_test_utils.hpp"
#include "test_sinks.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace DataTamer;
using DataTamerTest::logTimes;
using DataTamerTest::manual;
using DataTamerTest::ringOptions;
using DataTamerTest::ScratchDir;
using DataTamerTest::Source;
using std::chrono::milliseconds;
using std::chrono::seconds;

// A request that no snapshot has triggered yet is flushed around the newest timestamp of
// the current run. When the clock of a channel steps back by more than the window a new
// run starts: the maximum timestamp ever seen belongs to the old run, and the dump would
// hold only its snapshots.
TEST(MCAPRingSinkTimeJump, FlushOfAnUntriggeredRequestStaysOnTheNewRun)
{
  for(const bool by_stop : { false, true })
  {
    ScratchDir dir("ring_time_jump");
    const auto options = ringOptions(dir.file("jump.mcap"), seconds(10));
    auto sink = manual<MCAPRingSink>(options);
    Source source("jump", sink);
    for(const int64_t at : { 995, 996, 997, 998, 999, 1000, 1, 2, 3, 4, 5 })
    {
      source.take(seconds(at));
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
  const auto options = ringOptions(dir.file("jump.mcap"), seconds(3));
  auto sink = manual<MCAPRingSink>(options);
  Source a("a", sink);
  Source b("b", sink);
  a.take(seconds(1000));
  b.take(seconds(999));
  a.take(seconds(1));  // both clocks restart, and b lags behind a
  b.take(seconds(2));
  a.take(seconds(6));
  b.take(seconds(5));
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
  const auto options = ringOptions(dir.file("jump.mcap"), seconds(10));
  auto sink = manual<MCAPRingSink>(options);
  Source source("jump", sink);
  source.take(seconds(1000));
  sink.drain();
  ASSERT_TRUE(sink->requestDump(seconds(100)));
  source.take(seconds(1001));  // the trigger: the dump collects until 1101 s
  source.take(seconds(1));     // the clock steps back
  source.take(seconds(2));
  sink.drain();
  ASSERT_TRUE(sink->dumpRequested());

  ASSERT_TRUE(sink->flushPendingDump());

  // [991 s, 1001 s]: what the dump had when the clock stepped back
  EXPECT_EQ(logTimes(details::NumberedPath(options.filepath, 1)),
            (std::vector<uint64_t>{ 1'000'000'000'000, 1'001'000'000'000 }));
}

// Several threads can take snapshots of one channel, and a default timestamp is taken
// before the snapshot waits for the write mutex, so one channel can deliver its
// timestamps slightly out of order. That is no new run: a flush right after keeps the
// newest snapshot and makes it the trigger.
TEST(MCAPRingSinkTimeJump, FlushAfterSnapshotsSlightlyOutOfOrderKeepsTheNewest)
{
  ScratchDir dir("ring_out_of_order");
  const auto options = ringOptions(dir.file("order.mcap"), seconds(10));
  auto sink = manual<MCAPRingSink>(options);
  MCAPRingDump info;
  sink->setDumpCallback([&](const MCAPRingDump& dump) { info = dump; });
  int64_t value = 0;
  auto channel = channelWith(sink, &value);
  for(const auto at :
      { milliseconds(10'000), milliseconds(10'002), milliseconds(10'001) })
  {
    ASSERT_EQ(channel->takeSnapshot(at), SnapshotResult::ok);
  }
  sink.drain();

  ASSERT_TRUE(sink->requestDump());
  ASSERT_TRUE(sink->flushPendingDump());

  EXPECT_EQ(info.trigger_time, milliseconds(10'002));
  EXPECT_EQ(logTimes(details::NumberedPath(options.filepath, 1)),
            (std::vector<uint64_t>{ 10'000'000'000, 10'002'000'000, 10'001'000'000 }));
}

// A run ends only when a channel's clock steps back by more than the window, beyond
// which age eviction treats a snapshot as gone. Here the window is 4 s.
TEST(MCAPRingSinkTimeJump, OnlyAStepBackLongerThanTheWindowStartsANewRun)
{
  struct Case
  {
    std::vector<milliseconds> taken;
    std::vector<uint64_t> dumped;
  };
  const std::vector<Case> cases = {
    // 3 s back: one run, flushed around its newest timestamp, 100 s
    { { milliseconds(100'000), milliseconds(97'000) },
      { 100'000'000'000, 97'000'000'000 } },
    // exactly the window back: still one run
    { { milliseconds(100'000), milliseconds(96'000) },
      { 100'000'000'000, 96'000'000'000 } },
    // 4.001 s back: a new run, flushed around 95.999 s
    { { milliseconds(100'000), milliseconds(95'999) }, { 95'999'000'000 } },
  };
  for(const auto& c : cases)
  {
    SCOPED_TRACE("step back to " + std::to_string(c.taken.back().count()) + " ms");
    ScratchDir dir("ring_step_back");
    const auto options = ringOptions(dir.file("step.mcap"), seconds(4));
    auto sink = manual<MCAPRingSink>(options);
    int64_t value = 0;
    auto channel = channelWith(sink, &value);
    for(const auto at : c.taken)
    {
      ASSERT_EQ(channel->takeSnapshot(at), SnapshotResult::ok);
    }
    sink.drain();

    ASSERT_TRUE(sink->requestDump());
    ASSERT_TRUE(sink->flushPendingDump());

    EXPECT_EQ(logTimes(details::NumberedPath(options.filepath, 1)), c.dumped);
  }
}
