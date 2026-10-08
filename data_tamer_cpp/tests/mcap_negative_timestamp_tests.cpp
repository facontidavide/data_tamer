// MCAP times are unsigned: a negative snapshot timestamp is an error, not a time near
// the end of the 64-bit range.
#include "data_tamer/channel.hpp"
#include "data_tamer/sinks/mcap_encoding.hpp"
#include "data_tamer/sinks/mcap_ring_sink.hpp"
#include "data_tamer/sinks/mcap_sink.hpp"
#include "mcap_test_utils.hpp"
#include "test_sinks.hpp"

#include <gtest/gtest.h>
#include <mcap/writer.hpp>

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <vector>

using namespace DataTamer;
using DataTamerTest::channelWith;
using DataTamerTest::logTimes;
using DataTamerTest::manual;
using DataTamerTest::ScratchDir;
using DataTamerTest::summaryMessageCount;
using std::chrono::nanoseconds;
using std::chrono::seconds;

TEST(MCAPNegativeTimestamp, MCAPSinkCountsAnErrorAndDoesNotWriteTheMessage)
{
  ScratchDir dir("negative_mcap_sink");
  const auto path = dir.file("negative.mcap");
  auto sink = manual<MCAPSink>(path);
  int64_t value = 0;
  auto channel = channelWith(sink, &value);
  for(const int64_t at : { 5, -1, 7 })
  {
    ASSERT_EQ(channel->takeSnapshot(nanoseconds(at)), SnapshotResult::ok);
    sink.drain();
  }
  sink.worker->stop();

  EXPECT_EQ(sink.worker->errors(), 1u);
  EXPECT_EQ(summaryMessageCount(path), 2u);  // a wrapped -1 would be a third message
  EXPECT_EQ(logTimes(path), (std::vector<uint64_t>{ 5, 7 }));
}

TEST(MCAPNegativeTimestamp, MCAPRingSinkCountsAnErrorAndDoesNotStoreTheSnapshot)
{
  ScratchDir dir("negative_ring_sink");
  MCAPRingOptions options;
  options.filepath = dir.file("negative.mcap");
  options.window = seconds(10);
  options.capacity_bytes = 1 << 20;
  auto sink = manual<MCAPRingSink>(options);
  int64_t value = 0;
  auto channel = channelWith(sink, &value);
  for(const int64_t at : { 10, -1, 20 })
  {
    ASSERT_EQ(channel->takeSnapshot(nanoseconds(at)), SnapshotResult::ok);
  }
  sink.drain();
  EXPECT_EQ(sink.worker->errors(), 1u);
  EXPECT_EQ(sink->stats().stored_snapshots, 2u);

  ASSERT_TRUE(sink->requestDump());
  ASSERT_TRUE(sink->flushPendingDump());
  const auto dump = details::NumberedPath(options.filepath, 1);
  EXPECT_EQ(summaryMessageCount(dump), 2u);
  EXPECT_EQ(logTimes(dump), (std::vector<uint64_t>{ 10, 20 }));
}

TEST(MCAPNegativeTimestamp, WriteMessageThrowsInsteadOfWrappingTheTime)
{
  ScratchDir dir("negative_encoding");
  const auto path = dir.file("negative.mcap");
  auto channel = LogChannel::create("negative");
  int64_t value = 0;
  channel->registerValue("value", &value);
  mcap::McapWriter writer;
  ASSERT_TRUE(writer.open(path, mcap::McapWriterOptions(mcap_encoding::kEncoding)).ok());
  const auto id = mcap_encoding::AddChannel(writer, channel->getSchema());
  const std::vector<uint8_t> mask = { 0x01 };
  const std::vector<uint8_t> payload(sizeof(value), 0);
  std::vector<uint8_t> scratch;

  EXPECT_THROW(
      mcap_encoding::WriteMessage(writer, id, 1, nanoseconds(-1), mask, payload, scratch),
      std::invalid_argument);
  EXPECT_TRUE(
      mcap_encoding::WriteMessage(writer, id, 1, nanoseconds(0), mask, payload, scratch)
          .ok());
  writer.close();
  EXPECT_EQ(logTimes(path), (std::vector<uint64_t>{ 0 }));
}
