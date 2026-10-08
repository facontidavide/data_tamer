#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace DataTamerTest
{

/// `size` readable and writable bytes followed by an inaccessible page: an access past
/// the end faults, with or without a sanitizer. Without mmap it is a plain vector.
class GuardedBuffer
{
public:
  explicit GuardedBuffer(size_t size) : size_(size)
  {
#if defined(__unix__) || defined(__APPLE__)
    const auto page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    const size_t writable = (size + page - 1) / page * page;
    map_size_ = writable + page;
    map_ = mmap(nullptr, map_size_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                -1, 0);
    if(map_ == MAP_FAILED ||
       mprotect(static_cast<uint8_t*>(map_) + writable, page, PROT_NONE) != 0)
    {
      throw std::runtime_error("GuardedBuffer: mmap failed");
    }
    data_ = static_cast<uint8_t*>(map_) + writable - size;
#else
    fallback_.resize(size);
    data_ = fallback_.data();
#endif
  }

  /// A buffer holding a copy of `bytes`.
  explicit GuardedBuffer(const std::vector<uint8_t>& bytes) : GuardedBuffer(bytes.size())
  {
    if(!bytes.empty())
    {
      std::memcpy(data_, bytes.data(), bytes.size());
    }
  }

  GuardedBuffer(const GuardedBuffer&) = delete;
  GuardedBuffer& operator=(const GuardedBuffer&) = delete;

  ~GuardedBuffer()
  {
#if defined(__unix__) || defined(__APPLE__)
    munmap(map_, map_size_);
#endif
  }

  uint8_t* data() { return data_; }
  size_t size() const { return size_; }

private:
  size_t size_ = 0;
  uint8_t* data_ = nullptr;
#if defined(__unix__) || defined(__APPLE__)
  void* map_ = nullptr;
  size_t map_size_ = 0;
#else
  std::vector<uint8_t> fallback_;
#endif
};

}  // namespace DataTamerTest
