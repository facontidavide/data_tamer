#include "alloc_counter.hpp"

#include <cstdlib>
#include <new>

namespace DataTamerTest
{
thread_local bool AllocCounter::enabled = false;
thread_local std::size_t AllocCounter::allocations = 0;
thread_local std::size_t AllocCounter::deallocations = 0;
thread_local long AllocCounter::fail_countdown = 0;
thread_local bool AllocCounter::failed = false;
}  // namespace DataTamerTest

using DataTamerTest::AllocCounter;

namespace
{
/// True for the allocation a FailNth waits for.
bool injectFault() noexcept
{
  if(AllocCounter::fail_countdown > 0 && --AllocCounter::fail_countdown == 0)
  {
    AllocCounter::failed = true;
    return true;
  }
  return false;
}

/// Counts (when enabled) and allocates, or returns nullptr for a failing allocation;
/// never throws. Shared by every operator new overload so the size-0 rule, the counting
/// and the fault injection live in one place.
void* countedMalloc(std::size_t size) noexcept
{
  if(AllocCounter::enabled)
  {
    ++AllocCounter::allocations;
  }
  return injectFault() ? nullptr : std::malloc(size == 0 ? 1 : size);
}

void* countedAllocOrThrow(std::size_t size)
{
  void* p = countedMalloc(size);
  if(p == nullptr)
  {
    throw std::bad_alloc();
  }
  return p;
}

void* countedAlignedMalloc(std::size_t size, std::align_val_t alignment) noexcept
{
  if(AllocCounter::enabled)
  {
    ++AllocCounter::allocations;
  }
  if(injectFault())
  {
    return nullptr;
  }
  const auto align = static_cast<std::size_t>(alignment);
  const auto rounded = (size + align - 1) / align * align;  // aligned_alloc precondition
  return std::aligned_alloc(align, rounded == 0 ? align : rounded);
}

void* countedAlignedAllocOrThrow(std::size_t size, std::align_val_t alignment)
{
  void* p = countedAlignedMalloc(size, alignment);
  if(p == nullptr)
  {
    throw std::bad_alloc();
  }
  return p;
}

void countedFree(void* p) noexcept
{
  if(p != nullptr && AllocCounter::enabled)
  {
    ++AllocCounter::deallocations;
  }
  std::free(p);
}
}  // namespace

void* operator new(std::size_t size)
{
  return countedAllocOrThrow(size);
}
void* operator new[](std::size_t size)
{
  return countedAllocOrThrow(size);
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
  return countedMalloc(size);
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
  return countedMalloc(size);
}

void* operator new(std::size_t size, std::align_val_t al)
{
  return countedAlignedAllocOrThrow(size, al);
}
void* operator new[](std::size_t size, std::align_val_t al)
{
  return countedAlignedAllocOrThrow(size, al);
}
void* operator new(std::size_t size, std::align_val_t al, const std::nothrow_t&) noexcept
{
  return countedAlignedMalloc(size, al);
}
void* operator new[](std::size_t size, std::align_val_t al,
                     const std::nothrow_t&) noexcept
{
  return countedAlignedMalloc(size, al);
}

void operator delete(void* p, std::align_val_t) noexcept
{
  countedFree(p);
}
void operator delete[](void* p, std::align_val_t) noexcept
{
  countedFree(p);
}
void operator delete(void* p, std::size_t, std::align_val_t) noexcept
{
  countedFree(p);
}
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept
{
  countedFree(p);
}

void operator delete(void* p) noexcept
{
  countedFree(p);
}
void operator delete[](void* p) noexcept
{
  countedFree(p);
}
void operator delete(void* p, std::size_t) noexcept
{
  countedFree(p);
}
void operator delete[](void* p, std::size_t) noexcept
{
  countedFree(p);
}
