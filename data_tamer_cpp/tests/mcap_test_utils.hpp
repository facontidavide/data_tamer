#pragma once

#include "data_tamer_v2/channel.hpp"
#include "data_tamer_v2/sinks/mcap_ring_sink.hpp"

#include <mcap/reader.hpp>

#include <gtest/gtest.h>
#include <sys/resource.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace DataTamerTest
{
namespace detail
{
/// A path in the temporary directory that no other test, process or run uses:
/// data_tamer_<tag>_<pid>_<steady clock ticks>.
inline std::filesystem::path uniqueName(const std::string& tag)
{
  const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("data_tamer_" + tag + "_" + std::to_string(::getpid()) + "_" +
          std::to_string(ticks));
}
}  // namespace detail

/// A file path in the temporary directory that no other test, process or run uses.
inline std::string tempPath(const std::string& tag)
{
  return detail::uniqueName(tag).string() + ".mcap";
}

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

/// A new directory in the temporary directory, removed with its content on destruction.
class ScratchDir
{
public:
  explicit ScratchDir(const std::string& tag) : path_(detail::uniqueName(tag))
  {
    std::filesystem::create_directory(path_);
  }
  ~ScratchDir()
  {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  ScratchDir(const ScratchDir&) = delete;
  ScratchDir& operator=(const ScratchDir&) = delete;

  const std::filesystem::path& path() const { return path_; }
  std::string file(const std::string& name) const { return (path_ / name).string(); }

private:
  std::filesystem::path path_;
};

/// Number of messages in an MCAP file. With `finalized`, the file must also have
/// its summary and footer, which only a closed writer writes.
inline size_t countMessages(const std::string& path, bool finalized = false)
{
  mcap::McapReader reader;
  EXPECT_TRUE(reader.open(path).ok()) << path;
  if(finalized)
  {
    EXPECT_TRUE(reader.readSummary(mcap::ReadSummaryMethod::NoFallbackScan).ok()) << path;
  }
  size_t count = 0;
  for(const auto& message : reader.readMessages())
  {
    (void)message;
    ++count;
  }
  reader.close();
  return count;
}

/// Messages counted by the summary at the end of a finalized MCAP file.
inline uint64_t summaryMessageCount(const std::string& path)
{
  mcap::McapReader reader;
  EXPECT_TRUE(reader.open(path).ok()) << path;
  EXPECT_TRUE(reader.readSummary(mcap::ReadSummaryMethod::NoFallbackScan).ok()) << path;
  const uint64_t count = reader.statistics() ? reader.statistics()->messageCount : 0;
  reader.close();
  return count;
}

/// The log time of each message of an MCAP file, in file order.
inline std::vector<uint64_t> logTimes(const std::string& path)
{
  mcap::McapReader reader;
  EXPECT_TRUE(reader.open(path).ok()) << path;
  std::vector<uint64_t> times;
  for(const auto& message : reader.readMessages())
  {
    times.push_back(message.message.logTime);
  }
  reader.close();
  return times;
}

/// Options of a ring sink that dumps to `filepath`, keeps `window` of history and holds
/// `capacity_bytes` of it.
inline DataTamer2::MCAPRingOptions ringOptions(const std::string& filepath,
                                               std::chrono::nanoseconds window,
                                               size_t capacity_bytes = 1 << 20)
{
  DataTamer2::MCAPRingOptions options;
  options.filepath = filepath;
  options.window = window;
  options.capacity_bytes = capacity_bytes;
  return options;
}

/// The window in nanoseconds, as the ring tests count their timestamps.
inline DataTamer2::MCAPRingOptions ringOptions(const std::string& filepath,
                                               int64_t window_ns,
                                               size_t capacity_bytes = 1 << 20)
{
  return ringOptions(filepath, std::chrono::nanoseconds(window_ns), capacity_bytes);
}

/// One started channel named `name`, attached to `sink`, with an int64 "value" that
/// equals the timestamp of each snapshot.
struct Source
{
  std::shared_ptr<DataTamer2::LogChannel> channel;
  int64_t value = 0;

  Source(const std::string& name, const std::shared_ptr<DataTamer2::SinkWorker>& sink)
    : channel(DataTamer2::LogChannel::create(name))
  {
    channel->registerValue("value", &value);
    channel->addDataSink(sink);
    channel->startLogging();
  }
  Source(const Source&) = delete;
  Source& operator=(const Source&) = delete;

  void take(std::chrono::nanoseconds timestamp)
  {
    value = timestamp.count();
    ASSERT_EQ(channel->tryTakeSnapshot(timestamp), DataTamer2::SnapshotResult::ok);
  }
  void take(int64_t timestamp_ns) { take(std::chrono::nanoseconds(timestamp_ns)); }
};

}  // namespace DataTamerTest
