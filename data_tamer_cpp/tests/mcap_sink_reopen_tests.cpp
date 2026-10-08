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
#include <functional>
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

// What a case has to open the file in use again: the channel and the sink, the scratch
// directory and the path the sink is writing.
struct Scene
{
  const std::shared_ptr<LogChannel>& channel;
  const DataTamerTest::Attached<MCAPSink>& sink;
  const ScratchDir& dir;
  const std::string& path;
};

// Records 200 snapshots, opens the file in use again with `reopen`, records three more
// and reads the file back as an indexed reader does: it holds the three.
void expectNewRecordingReadable(const std::function<void(const Scene&)>& reopen)
{
  ScratchDir dir("reopen");
  const auto path = dir.file("current.mcap");
  uint64_t value = 7;
  auto sink = manual<MCAPSink>(path);
  auto channel = channelWith(sink, &value);

  take(channel, sink, 1, 200);
  reopen({ channel, sink, dir, path });
  take(channel, sink, 1001, 1003);
  sink.worker->stop();

  const auto recording = readThroughSummary(path);
  EXPECT_EQ(recording.summary_messages, 3u);
  EXPECT_EQ(recording.log_times, (std::vector<uint64_t>{ 1001, 1002, 1003 }));
  EXPECT_TRUE(recording.problems.empty()) << testing::PrintToString(recording.problems);
}
}  // namespace

// Opening the file in use truncates it. The old writer is closed first: if it were
// closed afterwards, its footer would land in the new file and the summary at the end
// would describe the previous recording.
TEST(MCAPSinkReopen, ResetWithoutRolloverLeavesTheNewRecordingReadable)
{
  expectNewRecordingReadable([](const Scene& scene) {
    scene.sink->setCreateNewFileOnReset(false);
    scene.sink->setMaxTimeBeforeReset(seconds(-1));  // the next snapshot resets the file
    take(scene.channel, scene.sink, 201, 201);
    scene.sink->setMaxTimeBeforeReset(seconds(0));
  });
}

TEST(MCAPSinkReopen, RestartOnTheCurrentPathLeavesTheNewRecordingReadable)
{
  expectNewRecordingReadable(
      [](const Scene& scene) { scene.sink->restartRecording(scene.path); });
}

// The file in use is recognised under another name too: here a link to it.
TEST(MCAPSinkReopen, RestartThroughALinkToTheCurrentFileLeavesTheNewRecordingReadable)
{
  expectNewRecordingReadable([](const Scene& scene) {
    const auto link = scene.dir.file("link.mcap");
    std::filesystem::create_symlink(scene.path, link);
    scene.sink->restartRecording(link);
  });
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
