#include "data_tamer/channel.hpp"
#include "data_tamer/sinks/dummy_sink.hpp"

#include <gtest/gtest.h>
#include <string>

using namespace DataTamer;

void take_snapshots(std::shared_ptr<LogChannel> channel, DummySink& sink, int count)
{
  for(int i = 0; i < count; i++)
  {
    channel->takeSnapshot();
  }
  sink.flush();
}

TEST(DataTamerSinkRegistry, AddSinkIncreasesCountAndRef)
{
  auto channel = LogChannel::create("chan");
  auto sink = std::make_shared<DummySink>();
  channel->addDataSink(sink);

  std::vector<double> dummyData = { 10, 11, 12 };
  channel->registerValue("valsA", &dummyData);

  ASSERT_EQ(channel->getNumberOfSinks(), 1);
}

TEST(DataTamerSinkRegistry, SnapshotsAreRecordedWhileSinkPresent)
{
  auto channel = LogChannel::create("chan");
  auto sink = std::make_shared<DummySink>();
  channel->addDataSink(sink);

  std::vector<double> dummyData = { 10, 11, 12 };
  channel->registerValue("valsA", &dummyData);

  const int snapshot_count = 10;
  take_snapshots(channel, *sink, snapshot_count);

  const auto hash = channel->getSchema().hash;
  ASSERT_EQ(sink->snapshotsCount(hash), snapshot_count);
}

TEST(DataTamerSinkRegistry, RemoveSinkStopsRecording)
{
  auto channel = LogChannel::create("chan");
  auto sink = std::make_shared<DummySink>();
  channel->addDataSink(sink);

  std::vector<double> dummyData = { 10, 11, 12 };
  channel->registerValue("valsA", &dummyData);

  const int snapshot_count = 10;
  take_snapshots(channel, *sink, snapshot_count);

  const auto hash = channel->getSchema().hash;
  ASSERT_EQ(sink->snapshotsCount(hash), snapshot_count);

  channel->removeDataSink(sink);

  ASSERT_EQ(channel->getNumberOfSinks(), 0);

  // Taking more snapshots, should not be recorded in the sink (i.e does not increase snapshots_count)
  take_snapshots(channel, *sink, snapshot_count);

  ASSERT_EQ(sink->snapshotsCount(hash), snapshot_count);
}
