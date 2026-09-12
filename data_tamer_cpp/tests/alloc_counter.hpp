#pragma once

#include <cstddef>

namespace DataTamerTest
{

/**
 * Per-thread allocation counter. Replaces the global operator new/delete
 * (see alloc_counter.cpp). Counting is active only while a Scope object is
 * alive on the current thread.
 */
struct AllocCounter
{
  static thread_local bool enabled;
  static thread_local std::size_t allocations;
  static thread_local std::size_t deallocations;

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
};

}  // namespace DataTamerTest
