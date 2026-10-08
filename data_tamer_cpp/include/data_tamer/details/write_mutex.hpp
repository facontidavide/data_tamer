#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <system_error>

#include "data_tamer/details/spin_pause.hpp"

// Priority inheritance is a POSIX option, not a Linux feature (QNX has it too).
// Without it WriteMutex is a plain std::mutex.
#if defined(__has_include)
#if __has_include(<unistd.h>)
#include <unistd.h>
#endif
#endif
#if defined(_POSIX_THREAD_PRIO_INHERIT) && _POSIX_THREAD_PRIO_INHERIT > 0
#include <pthread.h>
#define DATA_TAMER_HAS_PI_MUTEX 1
#else
#define DATA_TAMER_HAS_PI_MUTEX 0
#endif

namespace DataTamer
{
namespace details
{
#if DATA_TAMER_HAS_PI_MUTEX
/// pthread mutex with PTHREAD_PRIO_INHERIT. The constructor throws std::system_error
/// if the platform refuses it.
class PriorityInheritingMutex
{
public:
  PriorityInheritingMutex()
  {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    int rc = pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);
    if(rc == 0)
    {
      rc = pthread_mutex_init(&mutex_, &attr);
    }
    pthread_mutexattr_destroy(&attr);
    if(rc != 0)
    {
      throw std::system_error(rc, std::generic_category(),
                              "WriteMutex: priority-inheriting mutex not available");
    }
  }
  ~PriorityInheritingMutex() { pthread_mutex_destroy(&mutex_); }

  PriorityInheritingMutex(const PriorityInheritingMutex&) = delete;
  PriorityInheritingMutex& operator=(const PriorityInheritingMutex&) = delete;

  void lock() { pthread_mutex_lock(&mutex_); }
  bool try_lock() noexcept { return pthread_mutex_trylock(&mutex_) == 0; }
  void unlock() noexcept { pthread_mutex_unlock(&mutex_); }

private:
  pthread_mutex_t mutex_;
};
using PlatformWriteMutex = PriorityInheritingMutex;
#else
using PlatformWriteMutex = std::mutex;
#endif
}  // namespace details

/**
 * @brief Non-recursive mutex (C++ Lockable) shared by the writers of a channel and its
 * snapshot thread. Priority-inheriting where the platform supports it, which limits
 * priority inversion but gives no wait deadline.
 */
class WriteMutex
{
public:
  /// Default spin budget of tryLockWithSpin() and lockWithSpin(), in nanoseconds.
  static constexpr std::int64_t kLockSpinNs = 2000;

  WriteMutex() = default;

  WriteMutex(const WriteMutex&) = delete;
  WriteMutex& operator=(const WriteMutex&) = delete;
  WriteMutex(WriteMutex&&) = delete;
  WriteMutex& operator=(WriteMutex&&) = delete;

  void lock() { mutex_.lock(); }
  bool try_lock() noexcept { return mutex_.try_lock(); }
  void unlock() noexcept { mutex_.unlock(); }

  /// Spins on try_lock() for about spin_ns nanoseconds and never blocks. A budget that
  /// outlasts the clock spins until the lock is free.
  /// @return true if the lock was acquired.
  bool tryLockWithSpin(std::int64_t spin_ns = kLockSpinNs) noexcept
  {
    if(try_lock())
    {
      return true;
    }
    // Read the clock every kTriesPerClockCheck attempts: it costs more than a try_lock.
    constexpr int kTriesPerClockCheck = 8;
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    const std::chrono::nanoseconds budget(spin_ns);
    do
    {
      for(int i = 0; i < kTriesPerClockCheck; i++)
      {
        if(try_lock())
        {
          return true;
        }
        details::spinPause();
      }
    } while(Clock::now() - start < budget);
    return false;
  }

  /**
   * @brief Spins like tryLockWithSpin(), then blocks in lock().
   * @param blocked_wait_ns if not null, receives the nanoseconds spent in lock()
   *        (0 when the spin succeeded).
   * @return true if lock() was called; the thread may still not have slept.
   */
  bool lockWithSpin(std::int64_t spin_ns = kLockSpinNs,
                    std::uint64_t* blocked_wait_ns = nullptr)
  {
    if(blocked_wait_ns)
    {
      *blocked_wait_ns = 0;
    }
    if(tryLockWithSpin(spin_ns))
    {
      return false;
    }
    const auto wait_start = std::chrono::steady_clock::now();
    lock();
    if(blocked_wait_ns)
    {
      *blocked_wait_ns =
          std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - wait_start)
                            .count());
    }
    return true;
  }

private:
  details::PlatformWriteMutex mutex_;
};

}  // namespace DataTamer
