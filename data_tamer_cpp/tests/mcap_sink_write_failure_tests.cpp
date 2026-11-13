// MCAPSink on a file that cannot be written: the failure is reported, not a silently
// truncated file. The file size limit makes write() fail with EFBIG.
#include "data_tamer_v2/channel.hpp"
#include "data_tamer_v2/sinks/mcap_sink.hpp"
#include "mcap_test_utils.hpp"
#include "test_sinks.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

using namespace DataTamer2;
using DataTamerTest::channelWith;
using DataTamerTest::FileSizeLimit;
using DataTamerTest::manual;
using DataTamerTest::ScratchDir;

// Each snapshot is larger than an MCAP chunk, so the writer flushes it at once: the
// failed write() throws from onSnapshot(), counted in errors(), and so does every
// later snapshot of that file.
TEST(MCAPSinkWriteFailure, AFailedWriteIsCountedPerSnapshot)
{
  ScratchDir dir("mcap_write_failure");
  std::vector<uint8_t> blob(1 << 20, 7);
  auto sink = manual<MCAPSink>(dir.file("limited.mcap"));
  auto channel = channelWith(sink, &blob);
  channel->startLogging();
  {
    FileSizeLimit limit(4096);
    if(!limit.ok())
    {
      GTEST_SKIP() << "cannot lower the file size limit";
    }
    for(int i = 0; i < 3; ++i)
    {
      ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
      sink.drain();
    }
  }
  EXPECT_EQ(sink.worker->errors(), 3u);
  EXPECT_NE(sink.worker->lastError().find("MCAP write failed: write "), std::string::npos)
      << sink.worker->lastError();
}

// Data that only reaches the file when it is closed (the last chunk, the summary):
// the failure is thrown by stopRecording(), and by stop() through onStop().
TEST(MCAPSinkWriteFailure, StopRecordingReportsAFailedFinalWrite)
{
  ScratchDir dir("mcap_close_failure");
  std::vector<uint8_t> blob(10'000, 7);  // well below a chunk
  auto sink = manual<MCAPSink>(dir.file("limited.mcap"));
  auto channel = channelWith(sink, &blob);
  channel->startLogging();
  for(int i = 0; i < 3; ++i)
  {
    ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
    sink.drain();
  }
  FileSizeLimit limit(4096);
  if(!limit.ok())
  {
    GTEST_SKIP() << "cannot lower the file size limit";
  }
  EXPECT_EQ(sink.worker->errors(), 0u);
  EXPECT_THROW(sink->stopRecording(), std::runtime_error);
  EXPECT_NO_THROW(sink->stopRecording());  // already closed
}

// A file that failed is still rolled over: once the disk accepts writes again, the
// recording resumes in the next file instead of failing every later snapshot.
TEST(MCAPSinkWriteFailure, RolloverResumesRecordingAfterAFailedWrite)
{
  ScratchDir dir("mcap_write_failure_rollover");
  std::vector<uint8_t> blob(1 << 20, 7);  // larger than a chunk: written at once
  auto sink = manual<MCAPSink>(dir.file("rollover.mcap"));
  sink->setMaxTimeBeforeReset(
      std::chrono::seconds(-1));  // roll over after every snapshot
  auto channel = channelWith(sink, &blob);
  channel->startLogging();
  {
    FileSizeLimit limit(4096);
    if(!limit.ok())
    {
      GTEST_SKIP() << "cannot lower the file size limit";
    }
    ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
    sink.drain();
  }
  ASSERT_EQ(sink.worker->errors(), 1u);
  for(int i = 0; i < 3; ++i)
  {
    ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
    sink.drain();
  }
  EXPECT_EQ(sink.worker->errors(), 1u) << sink.worker->lastError();
  sink.worker->stop();
  EXPECT_EQ(sink.worker->errors(), 1u) << sink.worker->lastError();
}

// The documented contract of restartRecording(): a failure to close the previous file
// is thrown after the switch, and the new file records.
TEST(MCAPSinkWriteFailure, RestartRecordingThrowsTheCloseErrorAfterSwitching)
{
  ScratchDir dir("mcap_restart_close_failure");
  std::vector<uint8_t> blob(10'000, 7);
  auto sink = manual<MCAPSink>(dir.file("first.mcap"));
  auto channel = channelWith(sink, &blob);
  channel->startLogging();
  ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
  sink.drain();
  {
    FileSizeLimit limit(4096);
    if(!limit.ok())
    {
      GTEST_SKIP() << "cannot lower the file size limit";
    }
    EXPECT_THROW(sink->restartRecording(dir.file("second.mcap")), std::runtime_error);
  }
  ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
  sink.drain();
  sink.worker->stop();
  EXPECT_EQ(sink.worker->errors(), 0u) << sink.worker->lastError();
  EXPECT_EQ(DataTamerTest::countMessages(dir.file("second.mcap"), true), 1u);
}
