// DummySink bookkeeping.
#include "data_tamer/channel.hpp"
#include "data_tamer/sinks/dummy_sink.hpp"
#include "test_sinks.hpp"

#include <gtest/gtest.h>

using namespace DataTamer;
using DataTamerTest::channelWith;
using DataTamerTest::manual;

// The channel announces its schema again when the worker is attached again. The count of
// the snapshots already delivered must survive it.
TEST(DummySink, SnapshotCountSurvivesTheWorkerBeingAttachedAgain)
{
  auto sink = manual<DummySink>();
  double value = 1.0;
  auto channel = channelWith(sink, &value);
  for(int i = 0; i < 3; ++i)
  {
    ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
    sink.drain();
  }
  const auto hash = sink->firstSchemaHash();
  ASSERT_EQ(sink->snapshotsCount(hash), 3);

  channel->removeDataSink(sink.worker);
  ASSERT_TRUE(channel->addDataSink(sink.worker));
  ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
  sink.drain();

  EXPECT_EQ(sink->snapshotsCount(hash), 4);
  EXPECT_EQ(sink->schemasCount(), 1u);
}
