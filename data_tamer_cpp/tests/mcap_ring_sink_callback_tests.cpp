// The dump callback of MCAPRingSink and the counters it can read.
#include "data_tamer/channel.hpp"
#include "data_tamer/sinks/mcap_ring_sink.hpp"
#include "mcap_test_utils.hpp"
#include "test_sinks.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>

using namespace DataTamer;
using DataTamerTest::channelWith;
using DataTamerTest::manual;
using DataTamerTest::ScratchDir;
using std::chrono::nanoseconds;

namespace
{
struct Counters
{
  uint64_t written = 99;
  uint64_t failed = 99;
};

// Writes one dump to `path` and returns the counters its callback read.
Counters countersSeenByTheCallback(const std::string& path)
{
  MCAPRingOptions options;
  options.filepath = path;
  options.capacity_bytes = 1 << 20;
  auto sink = manual<MCAPRingSink>(options);
  Counters seen;
  sink->setDumpCallback([&](const MCAPRingDump&) {
    seen.written = sink->stats().dumps_written;
    seen.failed = sink->stats().dumps_failed;
  });
  int64_t value = 0;
  auto channel = channelWith(sink, &value);
  EXPECT_EQ(channel->takeSnapshot(nanoseconds(100)), SnapshotResult::ok);
  sink.drain();
  EXPECT_TRUE(sink->requestDump());
  EXPECT_EQ(channel->takeSnapshot(nanoseconds(200)), SnapshotResult::ok);  // trigger
  EXPECT_EQ(channel->takeSnapshot(nanoseconds(300)), SnapshotResult::ok);  // complete
  sink.drain();
  sink->waitForWriter();
  return seen;
}
}  // namespace

// The counters already include the dump when its callback runs, so a callback that logs
// dumps_written sees the dump it is reporting.
TEST(MCAPRingSinkCallback, CountersIncludeASuccessfulDump)
{
  ScratchDir dir("ring_callback_ok");
  const auto seen = countersSeenByTheCallback(dir.file("ok.mcap"));
  EXPECT_EQ(seen.written, 1u);
  EXPECT_EQ(seen.failed, 0u);
}

TEST(MCAPRingSinkCallback, CountersIncludeAFailedDump)
{
  ScratchDir dir("ring_callback_failed");
  const auto seen = countersSeenByTheCallback(dir.file("missing_directory/failed.mcap"));
  EXPECT_EQ(seen.written, 0u);
  EXPECT_EQ(seen.failed, 1u);
}
