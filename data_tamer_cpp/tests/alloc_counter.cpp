#include "alloc_counter.hpp"

#include <cstdlib>
#include <new>

namespace DataTamerTest
{
thread_local bool AllocCounter::enabled = false;
thread_local std::size_t AllocCounter::allocations = 0;
thread_local std::size_t AllocCounter::deallocations = 0;
}  // namespace DataTamerTest

using DataTamerTest::AllocCounter;

namespace
{
/// Counts (when enabled) and allocates; never throws. Shared by every
/// operator new overload so the size-0 rule and the counting live in one place.
void* countedMalloc(std::size_t size) noexcept
{
  if(AllocCounter::enabled)
  {
    ++AllocCounter::allocations;
  }
  return std::malloc(size == 0 ? 1 : size);
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

void* operator new(std::size_t size) { return countedAllocOrThrow(size); }
void* operator new[](std::size_t size) { return countedAllocOrThrow(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return countedMalloc(size); }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { return countedMalloc(size); }

void* operator new(std::size_t size, std::align_val_t al) { return countedAlignedAllocOrThrow(size, al); }
void* operator new[](std::size_t size, std::align_val_t al) { return countedAlignedAllocOrThrow(size, al); }
void* operator new(std::size_t size, std::align_val_t al, const std::nothrow_t&) noexcept { return countedAlignedMalloc(size, al); }
void* operator new[](std::size_t size, std::align_val_t al, const std::nothrow_t&) noexcept { return countedAlignedMalloc(size, al); }

void operator delete(void* p, std::align_val_t) noexcept { countedFree(p); }
void operator delete[](void* p, std::align_val_t) noexcept { countedFree(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { countedFree(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { countedFree(p); }

void operator delete(void* p) noexcept { countedFree(p); }
void operator delete[](void* p) noexcept { countedFree(p); }
void operator delete(void* p, std::size_t) noexcept { countedFree(p); }
void operator delete[](void* p, std::size_t) noexcept { countedFree(p); }
