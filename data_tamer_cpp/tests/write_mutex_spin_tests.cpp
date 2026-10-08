#include "data_tamer/details/write_mutex.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <thread>
#include <utility>

using namespace DataTamer;
using namespace std::chrono_literals;

// tryTakeSnapshot() calls the try paths, and must not throw.
static_assert(noexcept(std::declval<WriteMutex&>().try_lock()));
static_assert(noexcept(std::declval<WriteMutex&>().unlock()));
static_assert(noexcept(std::declval<WriteMutex&>().tryLockWithSpin()));

// now() + spin_ns overflowed the clock for a huge budget: the deadline wrapped into the
// past and the call gave up at once (undefined behaviour, reported by UBSan).
TEST(WriteMutexSpin, AHugeBudgetSpinsUntilTheLockIsFree)
{
  WriteMutex mutex;
  mutex.lock();
  std::atomic<bool> started{ false };
  std::atomic<bool> acquired{ false };
  std::thread spinner([&] {
    started = true;
    acquired = mutex.tryLockWithSpin(std::numeric_limits<std::int64_t>::max());
    if(acquired)
    {
      mutex.unlock();
    }
  });
  while(!started)
  {
    std::this_thread::yield();
  }
  std::this_thread::sleep_for(50ms);
  mutex.unlock();
  spinner.join();

  EXPECT_TRUE(acquired);
}

TEST(WriteMutexSpin, ABudgetThatIsNotPositiveTriesOnce)
{
  WriteMutex mutex;
  EXPECT_TRUE(mutex.tryLockWithSpin(0));
  mutex.unlock();
  EXPECT_TRUE(mutex.tryLockWithSpin(std::numeric_limits<std::int64_t>::min()));

  // Held by this thread, so the other thread cannot get it however it asks.
  bool acquired = true;
  std::thread([&] { acquired = mutex.tryLockWithSpin(-1); }).join();
  EXPECT_FALSE(acquired);
  std::thread([&] {
    acquired = mutex.tryLockWithSpin(std::numeric_limits<std::int64_t>::min());
  }).join();
  EXPECT_FALSE(acquired);
  mutex.unlock();
}

TEST(WriteMutexSpin, ABoundedBudgetGivesUpWhenTheLockStaysHeld)
{
  WriteMutex mutex;
  mutex.lock();
  bool acquired = true;
  const auto start = std::chrono::steady_clock::now();
  std::thread([&] { acquired = mutex.tryLockWithSpin(5'000'000); }).join();  // 5 ms
  const auto elapsed = std::chrono::steady_clock::now() - start;
  mutex.unlock();

  EXPECT_FALSE(acquired);
  EXPECT_GE(elapsed, 5ms);
}
