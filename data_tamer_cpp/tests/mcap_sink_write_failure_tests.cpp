// MCAPSink on a file that cannot be written: the failure is reported, not a silently
// truncated file. The file size limit makes write() fail with EFBIG.
#include "data_tamer_v2/channel.hpp"
#include "data_tamer_v2/sinks/mcap_sink.hpp"
#include "mcap_test_utils.hpp"
#include "test_sinks.hpp"

#include <gtest/gtest.h>
#include <sys/resource.h>

#include <csignal>
#include <cstdint>
#include <string>
#include <vector>

using namespace DataTamer2;
using DataTamerTest::channelWith;
using DataTamerTest::manual;
using DataTamerTest::ScratchDir;

namespace
{
/// Limits the size of the files this process writes to `bytes` and ignores SIGXFSZ,
/// restoring both on destruction, so that later tests can write files.
class FileSizeLimit
{
public:
  explicit FileSizeLimit(rlim_t bytes)
  {
    ok_ = ::getrlimit(RLIMIT_FSIZE, &original_) == 0 &&
          (original_.rlim_max == RLIM_INFINITY || original_.rlim_max >= bytes);
    if(!ok_)
    {
      return;
    }
    handler_ = std::signal(SIGXFSZ, SIG_IGN);
    active_ = true;
    rlimit limited = original_;
    limited.rlim_cur = bytes;
    ok_ = ::setrlimit(RLIMIT_FSIZE, &limited) == 0;
  }
  ~FileSizeLimit()
  {
    if(active_)  // handler_ may be SIG_DFL, a null pointer
    {
      ::setrlimit(RLIMIT_FSIZE, &original_);
      std::signal(SIGXFSZ, handler_);
    }
  }
  FileSizeLimit(const FileSizeLimit&) = delete;
  FileSizeLimit& operator=(const FileSizeLimit&) = delete;

  bool ok() const { return ok_; }

private:
  rlimit original_{};
  void (*handler_)(int) = nullptr;
  bool active_ = false;
  bool ok_ = false;
};
}  // namespace

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
