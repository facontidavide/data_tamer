#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace DataTamerTest
{
/// Parks a thread in pause() until the test calls release().
struct Gate
{
  void pause()
  {
    std::unique_lock lock(mutex);
    entered = true;
    cv.notify_all();
    cv.wait(lock, [&] { return released; });
  }
  /// True once a thread is parked; false after 5 s.
  bool waitEntered()
  {
    std::unique_lock lock(mutex);
    return cv.wait_for(lock, std::chrono::seconds(5), [&] { return entered; });
  }
  void release()
  {
    std::lock_guard lock(mutex);
    released = true;
    cv.notify_all();
  }

  std::mutex mutex;
  std::condition_variable cv;
  bool entered = false;
  bool released = false;
};
}  // namespace DataTamerTest
