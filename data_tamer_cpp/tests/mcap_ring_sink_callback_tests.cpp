// The dump callback of MCAPRingSink and the counters it can read.
#include "data_tamer/sinks/mcap_ring_sink.hpp"
#include "mcap_test_utils.hpp"
#include "test_sinks.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>

using namespace DataTamer;
using DataTamerTest::manual;
using DataTamerTest::ringOptions;
using DataTamerTest::ScratchDir;
using DataTamerTest::Source;
using std::chrono::seconds;

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
  auto sink = manual<MCAPRingSink>(ringOptions(path, seconds(10)));
  Counters seen;
  sink->setDumpCallback([&](const MCAPRingDump&) {
    seen.written = sink->stats().dumps_written;
    seen.failed = sink->stats().dumps_failed;
  });
  Source source("ring_callback", sink);
  source.take(100);
  sink.drain();
  EXPECT_TRUE(sink->requestDump());
  source.take(200);  // trigger
  source.take(300);  // complete
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
