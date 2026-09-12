#include "alloc_counter.hpp"

#include <gtest/gtest.h>

#include <thread>
#include <vector>

using DataTamerTest::AllocCounter;

TEST(AllocCounter, CountsNewAndDeleteInsideScope)
{
  AllocCounter::Scope scope;
  {
    std::vector<int> v(100);
    ASSERT_EQ(scope.allocations(), 1u);
    ASSERT_EQ(scope.deallocations(), 0u);
  }
  ASSERT_EQ(scope.allocations(), 1u);
  ASSERT_EQ(scope.deallocations(), 1u);
}

TEST(AllocCounter, DoesNotCountOutsideScope)
{
  {
    AllocCounter::Scope scope;
  }
  // scope ended: this allocation must not be attributed
  std::vector<int> v(100);
  ASSERT_EQ(AllocCounter::allocations, 0u);   // raw thread_local, read BEFORE any new Scope resets it
  ASSERT_FALSE(AllocCounter::enabled);
  AllocCounter::Scope scope;
  ASSERT_EQ(scope.allocations(), 0u);
}

TEST(AllocCounter, IsPerThread)
{
  AllocCounter::Scope scope;
  std::thread t([] {
    std::vector<int> v(1000);  // allocation on another thread
    (void)v;
  });
  const std::size_t after_create = scope.allocations();  // std::thread's own state alloc(s) happen here, on this thread
  ASSERT_GT(after_create, 0u);
  t.join();
  // nothing the child thread allocated may be attributed to this thread
  ASSERT_EQ(scope.allocations(), after_create);
}

TEST(AllocCounter, ReuseOfCapacityDoesNotAllocate)
{
  std::vector<uint8_t> payload;
  payload.reserve(4096);
  AllocCounter::Scope scope;
  for(int i = 0; i < 1000; i++)
  {
    payload.resize(100 + (i % 10));
  }
  ASSERT_EQ(scope.allocations(), 0u);
}

TEST(AllocCounter, CountsOverAlignedAllocations)
{
  struct alignas(64) Wide
  {
    char bytes[64];
  };
  AllocCounter::Scope scope;
  auto* one = new Wide;
  auto* many = new Wide[3];
  // Observe the pointers, or the optimizer elides the unused new/delete pairs.
  asm volatile("" : : "g"(one), "g"(many) : "memory");
  ASSERT_EQ(scope.allocations(), 2u);
  delete one;
  delete[] many;
  ASSERT_EQ(scope.deallocations(), 2u);
}
