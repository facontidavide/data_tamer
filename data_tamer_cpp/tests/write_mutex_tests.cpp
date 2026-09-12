#include "data_tamer/details/write_mutex.hpp"
#include "wait_for_sleeping_thread.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

using DataTamer::WriteMutex;

TEST(WriteMutex, IsLockable)
{
  WriteMutex m;
  {
    std::lock_guard<WriteMutex> lk(m);
  }
  {
    std::unique_lock<WriteMutex> lk(m);
    ASSERT_TRUE(lk.owns_lock());
  }
  {
    std::scoped_lock lk(m);
  }
}

TEST(WriteMutex, TryLockFailsWhileHeld)
{
  WriteMutex m;
  m.lock();
  std::atomic_bool other_got_it{ true };
  std::thread t([&] { other_got_it = m.try_lock(); });
  t.join();
  ASSERT_FALSE(other_got_it);
  m.unlock();
  ASSERT_TRUE(m.try_lock());
  m.unlock();
}

TEST(WriteMutex, LockWithSpinReturnsFalseWhenUncontended)
{
  WriteMutex m;
  uint64_t waited = 123;
  EXPECT_FALSE(m.lockWithSpin(WriteMutex::kLockSpinNs, &waited));
  EXPECT_EQ(waited, 0u);
  m.unlock();
}

TEST(WriteMutex, LockWithSpinOwnsMutexAndReportsWaitAfterHandoff)
{
  for(int64_t budget : { int64_t(0), WriteMutex::kLockSpinNs, int64_t(50'000'000) })
  {
    WriteMutex m;
    m.lock();
    std::atomic_bool started{ false }, acquired{ false }, release{ false };
    bool blocked = false;
    uint64_t waited = 123, elapsed = 0;
    std::thread waiter([&] {
      started = true;
      const auto before = std::chrono::steady_clock::now();
      blocked = m.lockWithSpin(budget, &waited);
      elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - before)
                    .count();
      acquired = true;
      while(!release)
        std::this_thread::yield();
      m.unlock();
    });
    while(!started)
      std::this_thread::yield();
    EXPECT_FALSE(acquired.load());  // Cannot acquire while this thread owns it.
    m.unlock();
    while(!acquired)
      std::this_thread::yield();
    const bool stolen = m.try_lock();
    EXPECT_FALSE(stolen);  // Returning from lockWithSpin must convey ownership.
    if(stolen)
      m.unlock();
    release = true;
    waiter.join();

    // The scheduler may run the waiter before or after the handoff, regardless
    // of budget. Validate the reported path, not a presumed scheduling delay.
    if(!blocked)
      EXPECT_EQ(waited, 0u);
    EXPECT_LE(waited, elapsed);
    const bool reusable = m.try_lock();
    EXPECT_TRUE(reusable);
    if(reusable)
      m.unlock();
  }
}

TEST(WriteMutex, ObservedSleepingWaiterReportsBlockingFallback)
{
#if defined(__linux__)
  if(!std::ifstream("/proc/self/stat"))
    GTEST_SKIP() << "needs readable procfs";
  WriteMutex m;
  m.lock();
  std::atomic<pid_t> tid{ 0 };
  std::atomic_bool finished{ false };
  bool blocked = false;
  uint64_t waited = 0;
  std::thread waiter([&] {
    tid = static_cast<pid_t>(syscall(SYS_gettid));
    blocked = m.lockWithSpin(WriteMutex::kLockSpinNs, &waited);
    m.unlock();
    finished = true;
  });
  const bool observed = DataTamerTest::waitForSleepingThread(tid, finished);
  m.unlock();
  waiter.join();  // Release and join before any fatal assertion, including timeout.
  ASSERT_TRUE(observed) << "waiter did not sleep on the held mutex";
  EXPECT_TRUE(blocked);
  EXPECT_GT(waited, 0u);
#else
  GTEST_SKIP() << "observing a blocked waiter requires Linux procfs";
#endif
}

// Privileged priority-inheritance observation. Needs CAP_SYS_NICE; skipped otherwise.
// A SCHED_OTHER holder shares one core with three SCHED_OTHER CPU hogs, so
// without priority inheritance it gets ~1/4 of the core and its 200 us of
// work spans several CFS slices (milliseconds). With PI, the SCHED_FIFO
// waiter boosts the holder the moment it blocks, so the wait is the holder's
// remaining work plus a wake-up. This is not a universal deadline.
//
// Note: the FIFO waiter must never spin-wait on this core (a spinning FIFO
// thread starves every CFS thread on it, including the holder); it sleeps.
TEST(WriteMutex, PriorityInheritanceKeepsObservedWaitShort)
{
#if defined(__linux__)
  sched_param fifo{};
  fifo.sched_priority = 50;
  if(pthread_setschedparam(pthread_self(), SCHED_FIFO, &fifo) != 0)
  {
    GTEST_SKIP() << "needs CAP_SYS_NICE for SCHED_FIFO";
  }
  struct RestoreScheduler
  {
    cpu_set_t original_affinity;
    RestoreScheduler()
    {
      pthread_getaffinity_np(pthread_self(), sizeof(original_affinity),
                             &original_affinity);
    }
    ~RestoreScheduler()
    {
      sched_param other{};
      pthread_setschedparam(pthread_self(), SCHED_OTHER, &other);
      pthread_setaffinity_np(pthread_self(), sizeof(original_affinity),
                             &original_affinity);
    }
  } restore;

  cpu_set_t one_core;
  CPU_ZERO(&one_core);
  CPU_SET(0, &one_core);
  if(pthread_setaffinity_np(pthread_self(), sizeof(one_core), &one_core) != 0)
  {
    GTEST_SKIP() << "cannot pin to CPU 0";
  }

  WriteMutex m;
  std::atomic_bool stop{ false };
  std::atomic_bool holder_has_lock{ false };

  auto become_cfs_on_core0 = [&] {
    sched_param other{};
    pthread_setschedparam(pthread_self(), SCHED_OTHER, &other);
    pthread_setaffinity_np(pthread_self(), sizeof(one_core), &one_core);
  };

  std::vector<std::thread> hogs;
  for(int i = 0; i < 3; i++)
  {
    hogs.emplace_back([&] {
      become_cfs_on_core0();
      while(!stop)
      {
      }
    });
  }

  std::thread holder([&] {
    become_cfs_on_core0();
    m.lock();
    holder_has_lock = true;
    // 200 us of CPU work while holding the mutex
    const auto t0 = std::chrono::steady_clock::now();
    while(std::chrono::steady_clock::now() - t0 < std::chrono::microseconds(200))
    {
    }
    m.unlock();
  });

  while(!holder_has_lock)
  {
    std::this_thread::sleep_for(std::chrono::microseconds(50));  // let CFS threads run
  }
  const auto t0 = std::chrono::steady_clock::now();
  m.lock();
  const auto waited = std::chrono::steady_clock::now() - t0;
  m.unlock();
  stop = true;
  holder.join();
  for(auto& h : hogs)
  {
    h.join();
  }
  ASSERT_LT(std::chrono::duration_cast<std::chrono::microseconds>(waited).count(), 1000)
      << "observed wait "
      << std::chrono::duration_cast<std::chrono::microseconds>(waited).count() << " us";
#else
  GTEST_SKIP() << "priority-inheritance observation requires Linux";
#endif
}
