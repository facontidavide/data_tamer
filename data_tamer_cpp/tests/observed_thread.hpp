#pragma once

#if defined(__linux__)
#include "wait_for_sleeping_thread.hpp"

#include <atomic>
#include <thread>
#include <sys/syscall.h>
#include <unistd.h>

namespace DataTamerTest
{

/// A thread whose blocking a test observes through procfs; joined on destruction.
class ObservedThread
{
public:
  template <typename Function>
  explicit ObservedThread(Function function)
    : thread_([this, function] {
      tid_ = static_cast<pid_t>(syscall(SYS_gettid));
      function();
      finished_ = true;
    })
  {}
  ~ObservedThread() { thread_.join(); }
  ObservedThread(const ObservedThread&) = delete;
  ObservedThread& operator=(const ObservedThread&) = delete;

  /// True once the thread sleeps (blocked), false if it finishes first.
  bool sleeps() { return waitForSleepingThread(tid_, finished_); }

private:
  std::atomic<pid_t> tid_{ 0 };
  std::atomic<bool> finished_{ false };
  std::thread thread_;
};

}  // namespace DataTamerTest
#endif
