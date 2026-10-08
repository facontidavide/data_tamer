// MCAPSink opening the file it is writing again: a reset without rollover and
// restartRecording() on the current path.
#include "data_tamer/channel.hpp"
#include "data_tamer/sinks/mcap_sink.hpp"
#include "mcap_test_utils.hpp"
#include "test_sinks.hpp"

#include <gtest/gtest.h>
#include <mcap/reader.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace DataTamer;
using DataTamerTest::channelWith;
using DataTamerTest::manual;
using DataTamerTest::ScratchDir;
using std::chrono::nanoseconds;
using std::chrono::seconds;

namespace
{
struct Recording
{
  size_t summary_messages = 0;      // Statistics record found through the footer
  std::vector<uint64_t> log_times;  // messages found through the chunk index
  std::vector<std::string> problems;
};

// Reads as an indexed reader does: footer, summary, then the chunks it points to.
Recording readThroughSummary(const std::string& path)
{
  Recording recording;
  mcap::McapReader reader;
  EXPECT_TRUE(reader.open(path).ok()) << path;
  const auto summary = reader.readSummary(mcap::ReadSummaryMethod::NoFallbackScan);
  EXPECT_TRUE(summary.ok()) << path << ": " << summary.message;
  if(reader.statistics())
  {
    recording.summary_messages = reader.statistics()->messageCount;
  }
  mcap::ReadMessageOptions options;
  options.readOrder = mcap::ReadMessageOptions::ReadOrder::LogTimeOrder;
  const auto on_problem = [&](const mcap::Status& status) {
    recording.problems.push_back(status.message);
  };
  for(const auto& view : reader.readMessages(on_problem, options))
  {
    recording.log_times.push_back(view.message.logTime);
  }
  reader.close();
  return recording;
}

// Takes snapshots stamped first..last, one per nanosecond, delivering each at once.
void take(const std::shared_ptr<LogChannel>& channel,
          const DataTamerTest::Attached<MCAPSink>& sink, int64_t first, int64_t last)
{
  for(int64_t stamp = first; stamp <= last; ++stamp)
  {
    ASSERT_EQ(channel->takeSnapshot(nanoseconds(stamp)), SnapshotResult::ok);
    sink.drain();
  }
}
}  // namespace

// Opening the file in use truncates it. The old writer is closed first: if it were
// closed afterwards, its footer would land in the new file and the summary at the end
// would describe the previous recording.
TEST(MCAPSinkReopen, ResetWithoutRolloverLeavesTheNewRecordingReadable)
{
  ScratchDir dir("reopen_reset");
  const auto path = dir.file("reset.mcap");
  uint64_t value = 7;
  auto sink = manual<MCAPSink>(path);
  sink->setCreateNewFileOnReset(false);
  sink->setMaxTimeBeforeReset(seconds(0));
  auto channel = channelWith(sink, &value);

  take(channel, sink, 1, 200);
  sink->setMaxTimeBeforeReset(seconds(-1));  // the next snapshot resets the file
  take(channel, sink, 201, 201);
  sink->setMaxTimeBeforeReset(seconds(0));
  take(channel, sink, 1001, 1003);
  sink.worker->stop();

  const auto recording = readThroughSummary(path);
  EXPECT_EQ(recording.summary_messages, 3u);
  EXPECT_EQ(recording.log_times, (std::vector<uint64_t>{ 1001, 1002, 1003 }));
  EXPECT_TRUE(recording.problems.empty()) << testing::PrintToString(recording.problems);
}

TEST(MCAPSinkReopen, RestartOnTheCurrentPathLeavesTheNewRecordingReadable)
{
  ScratchDir dir("reopen_restart");
  const auto path = dir.file("restart.mcap");
  uint64_t value = 7;
  auto sink = manual<MCAPSink>(path);
  auto channel = channelWith(sink, &value);

  take(channel, sink, 1, 200);
  sink->restartRecording(path);
  take(channel, sink, 1001, 1003);
  sink.worker->stop();

  const auto recording = readThroughSummary(path);
  EXPECT_EQ(recording.summary_messages, 3u);
  EXPECT_EQ(recording.log_times, (std::vector<uint64_t>{ 1001, 1002, 1003 }));
  EXPECT_TRUE(recording.problems.empty()) << testing::PrintToString(recording.problems);
}

// The file in use is recognised under another name too: here a link to it.
TEST(MCAPSinkReopen, RestartThroughALinkToTheCurrentFileLeavesTheNewRecordingReadable)
{
  ScratchDir dir("reopen_link");
  const auto path = dir.file("current.mcap");
  const auto link = dir.file("link.mcap");
  uint64_t value = 7;
  auto sink = manual<MCAPSink>(path);
  auto channel = channelWith(sink, &value);
  std::filesystem::create_symlink(path, link);

  take(channel, sink, 1, 200);
  sink->restartRecording(link);
  take(channel, sink, 1001, 1003);
  sink.worker->stop();

  const auto recording = readThroughSummary(path);
  EXPECT_EQ(recording.summary_messages, 3u);
  EXPECT_EQ(recording.log_times, (std::vector<uint64_t>{ 1001, 1002, 1003 }));
  EXPECT_TRUE(recording.problems.empty()) << testing::PrintToString(recording.problems);
}

// The file in use is closed before it is opened again, so when that open fails the
// sink is stopped: later snapshots are dropped without a crash, and a restart on a
// path that opens resumes recording.
TEST(MCAPSinkReopen, FailedReopenOfTheCurrentPathStopsTheSink)
{
  ScratchDir dir("reopen_failed");
  const auto path = dir.file("failed.mcap");
  uint64_t value = 7;
  auto sink = manual<MCAPSink>(path);
  auto channel = channelWith(sink, &value);
  take(channel, sink, 1, 3);

  std::filesystem::remove(path);            // the sink keeps writing to the unlinked file
  std::filesystem::create_directory(path);  // and the path cannot be opened as a file
  EXPECT_THROW(sink->restartRecording(path), std::runtime_error);
  take(channel, sink, 4, 5);
  EXPECT_EQ(sink.worker->errors(), 0u);

  std::filesystem::remove(path);
  sink->restartRecording(path);
  take(channel, sink, 1001, 1002);
  sink.worker->stop();
  EXPECT_EQ(readThroughSummary(path).log_times, (std::vector<uint64_t>{ 1001, 1002 }));
}
