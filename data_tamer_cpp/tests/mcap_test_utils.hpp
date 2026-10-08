#pragma once

#include <mcap/reader.hpp>

#include <gtest/gtest.h>
#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace DataTamerTest
{

/// A file path in the temporary directory that no other test, process or run
/// uses: data_tamer_<tag>_<pid>_<steady clock ticks>.mcap.
inline std::string tempPath(const std::string& tag)
{
  const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
  return (std::filesystem::temp_directory_path() /
          ("data_tamer_" + tag + "_" + std::to_string(::getpid()) + "_" +
           std::to_string(ticks) + ".mcap"))
      .string();
}

/// A new directory in the temporary directory, removed with its content on destruction.
class ScratchDir
{
public:
  explicit ScratchDir(const std::string& tag)
  {
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("data_tamer_" + tag + "_" + std::to_string(::getpid()) + "_" +
             std::to_string(ticks));
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

}  // namespace DataTamerTest
