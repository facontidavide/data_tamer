#pragma once

#include <mcap/reader.hpp>

#include <gtest/gtest.h>
#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>

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

}  // namespace DataTamerTest
