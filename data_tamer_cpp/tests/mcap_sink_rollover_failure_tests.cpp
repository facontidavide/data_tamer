// MCAPSink rollover that cannot open its next file.
#include "data_tamer/channel.hpp"
#include "data_tamer/sinks/mcap_sink.hpp"
#include "mcap_test_utils.hpp"
#include "test_sinks.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>

using namespace DataTamer;
using DataTamerTest::channelWith;
using DataTamerTest::countMessages;
using DataTamerTest::manual;
using DataTamerTest::ScratchDir;
using std::chrono::seconds;

namespace
{
// The rollover takes `name` for a free name, because nothing exists there, but cannot
// open it: it is a link to a file in a directory that does not exist. Unlike a read-only
// directory this fails for every user, root included.
void makeUnopenable(const ScratchDir& dir, const std::string& name)
{
  std::filesystem::create_symlink(dir.path() / "missing" / "target.mcap",
                                  dir.path() / name);
}
}  // namespace

// The failure is counted once, the snapshots keep going to the current file, and the
// next attempt waits for another reset time. A reset time left expired would make every
// later snapshot try again, fail again and count another error.
TEST(MCAPSinkRolloverFailure, IsCountedOnceAndTheCurrentFileKeepsRecording)
{
  ScratchDir dir("rollover_failure");
  const auto path = dir.file("rollover.mcap");
  makeUnopenable(dir, "rollover_1.mcap");
  uint64_t value = 7;
  auto sink = manual<MCAPSink>(path);
  sink->setMaxTimeBeforeReset(seconds(1));
  auto channel = channelWith(sink, &value);

  std::this_thread::sleep_for(std::chrono::milliseconds(1100));  // the file is too old
  const auto start = std::chrono::steady_clock::now();
  for(int i = 0; i < 4; ++i)
  {
    ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
    sink.drain();
    // Only a stalled machine gets to the second attempt: it needs a full reset time
    // after the first.
    if(std::chrono::steady_clock::now() - start < seconds(1))
    {
      EXPECT_EQ(sink.worker->errors(), 1u) << "after snapshot " << i;
    }
  }
  sink.worker->stop();

  EXPECT_GE(sink.worker->errors(), 1u);
  EXPECT_EQ(countMessages(path, true), 4u);
}

// A failed rollover does not stop the next ones: once the file can be opened, the
// sink rolls over into it.
TEST(MCAPSinkRolloverFailure, RollsOverAgainOnceTheFileCanBeOpened)
{
  ScratchDir dir("rollover_recovers");
  const auto path = dir.file("rollover.mcap");
  makeUnopenable(dir, "rollover_1.mcap");
  uint64_t value = 7;
  auto sink = manual<MCAPSink>(path);
  sink->setMaxTimeBeforeReset(seconds(-1));  // every snapshot is due for a rollover
  auto channel = channelWith(sink, &value);

  ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
  sink.drain();
  ASSERT_EQ(sink.worker->errors(), 1u);  // rollover_1.mcap cannot be opened
  std::filesystem::remove(dir.path() / "rollover_1.mcap");
  for(int i = 0; i < 2; ++i)
  {
    ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
    sink.drain();
  }
  sink.worker->stop();

  EXPECT_EQ(sink.worker->errors(), 1u);
  EXPECT_EQ(countMessages(path, true), 2u);                         // up to the rollover
  EXPECT_EQ(countMessages(dir.file("rollover_1.mcap"), true), 1u);  // after it
}
