#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <system_error>

// Priority inheritance is a POSIX option, not a Linux feature: the same test
// admits QNX and other POSIX real-time systems. Without it WriteMutex is a plain
// std::mutex and blocking waits have no priority-inheritance mitigation.
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
/// pthread mutex created with PTHREAD_PRIO_INHERIT. The only place that
/// touches the platform primitive.
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
  bool try_lock() { return pthread_mutex_trylock(&mutex_) == 0; }
  void unlock() { pthread_mutex_unlock(&mutex_); }

private:
  pthread_mutex_t mutex_;
};
using PlatformWriteMutex = PriorityInheritingMutex;
#else
using PlatformWriteMutex = std::mutex;
#endif
}  // namespace details

/**
 * @brief Exclusive mutex with priority inheritance (PTHREAD_PRIO_INHERIT) where
 * the platform supports it. Satisfies the C++ Lockable requirements.
 *
 * Shared by the writer threads of a channel and by the snapshot thread. A
 * writer holding it can be boosted to the priority of a waiter, mitigating
 * priority inversion. Writer work and normal scheduling still provide no
 * universal wait deadline.
 */
class WriteMutex
{
public:
  static constexpr bool kPriorityInheritance = (DATA_TAMER_HAS_PI_MUTEX == 1);

  /// Nominal spin budget used by lockWithSpin() before blocking acquisition.
  static constexpr std::int64_t kLockSpinNs = 2000;

  WriteMutex() = default;

  WriteMutex(const WriteMutex&) = delete;
  WriteMutex& operator=(const WriteMutex&) = delete;
  WriteMutex(WriteMutex&&) = delete;
  WriteMutex& operator=(WriteMutex&&) = delete;

  void lock() { mutex_.lock(); }
  bool try_lock() { return mutex_.try_lock(); }
  void unlock() { mutex_.unlock(); }

  /**
   * @brief Spin on try_lock() for a nominal spin_ns budget, then call lock().
   * @return true when the blocking acquisition path was used. This does not
   *         guarantee that the kernel put the caller to sleep.
   */
  bool lockWithSpin(std::int64_t spin_ns = kLockSpinNs,
                    std::uint64_t* blocked_wait_ns = nullptr)
  {
    if(blocked_wait_ns)
    {
      *blocked_wait_ns = 0;
    }
    if(try_lock())
    {
      return false;
    }
    // The clock is read once per kTriesPerClockCheck attempts: a try_lock is a
    // few ns, a clock read tens of ns, so checking every iteration would spend
    // most of the budget on the clock instead of on the lock. A pause between
    // attempts keeps the spinner from stealing the mutex's cache line from the
    // owner it is waiting for.
    constexpr int kTriesPerClockCheck = 8;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::nanoseconds(spin_ns);
    do
    {
      for(int i = 0; i < kTriesPerClockCheck; i++)
      {
        if(try_lock())
        {
          return false;
        }
        spinPause();
      }
    } while(std::chrono::steady_clock::now() < deadline);
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
  static void spinPause()
  {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#endif
  }

  details::PlatformWriteMutex mutex_;
};

}  // namespace DataTamer
