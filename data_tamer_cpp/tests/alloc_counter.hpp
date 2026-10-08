#pragma once

#include <cstddef>

namespace DataTamerTest
{

/**
 * Per-thread allocation counter and fault injector. Replaces the global operator
 * new/delete (see alloc_counter.cpp). Counting is active only while a Scope object is
 * alive on the current thread, and a FailNth makes one allocation of the thread fail.
 */
struct AllocCounter
{
  static thread_local bool enabled;
  static thread_local std::size_t allocations;
  static thread_local std::size_t deallocations;
  static thread_local long fail_countdown;  // allocations until the failing one; 0: off
  static thread_local bool failed;          // the failing allocation has happened

  struct Scope
  {
    Scope()
    {
      AllocCounter::allocations = 0;
      AllocCounter::deallocations = 0;
      AllocCounter::enabled = true;
    }
    ~Scope() { AllocCounter::enabled = false; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

    std::size_t allocations() const { return AllocCounter::allocations; }
    std::size_t deallocations() const { return AllocCounter::deallocations; }
  };

  /// Makes the nth allocation of this thread from now on fail, while the object lives:
  /// operator new throws std::bad_alloc and the nothrow forms return nullptr. Code that
  /// must not fail an allocation, such as a gtest assertion, runs after disarm() or
  /// after the scope.
  struct FailNth
  {
    explicit FailNth(long nth)
    {
      AllocCounter::failed = false;
      AllocCounter::fail_countdown = nth;
    }
    ~FailNth() { disarm(); }
    FailNth(const FailNth&) = delete;
    FailNth& operator=(const FailNth&) = delete;

    void disarm() { AllocCounter::fail_countdown = 0; }
    /// True if the allocation that was to fail has happened.
    bool fired() const { return AllocCounter::failed; }
  };
};

}  // namespace DataTamerTest
