#pragma once

#if defined(__linux__)
#include <atomic>
#include <chrono>
#include <fstream>
#include <string>
#include <thread>
#include <sys/syscall.h>
#include <unistd.h>

namespace DataTamerTest
{
// The caller keeps the tested mutex locked. Between publishing tid and setting
// finished, that mutex must be the waiter's only possible blocking operation.
inline bool waitForSleepingThread(const std::atomic<pid_t>& tid,
                                  const std::atomic<bool>& finished)
{
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while(!finished && std::chrono::steady_clock::now() < deadline)
  {
    if(const auto id = tid.load())
    {
      std::ifstream stat("/proc/self/task/" + std::to_string(id) + "/stat");
      std::string line;
      std::getline(stat, line);
      // comm is parenthesized and may itself contain spaces or parentheses.
      const auto end = line.rfind(')');
      if(end != std::string::npos && end + 2 < line.size() && line[end + 2] == 'S')
        return true;
    }
    std::this_thread::yield();
  }
  return false;
}
}  // namespace DataTamerTest
#endif
