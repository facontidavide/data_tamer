#pragma once

#include "data_tamer/data_sink.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace DataTamer
{

/// One pre-allocated snapshot plus its intrusive reference count.
/// refs == 0 means free. Only the snapshot thread makes the 0 -> 1 transition.
///
/// The counter is the only field touched concurrently by sinks (release) and
/// the snapshot thread (acquire); it gets its own cache line so that traffic
/// never invalidates the snapshot data next to it. The snapshot itself is
/// written only by the snapshot thread while refs == 1.
struct PoolSlot
{
  Snapshot snapshot;
  alignas(64) std::atomic<uint32_t> refs{ 0 };
};

/**
 * @brief Fixed-size pool of PoolSlot, owned by a LogChannel (shared with every
 * SnapshotRef handed to a sink). Allocation happens only in the constructor.
 */
class SnapshotPool
{
public:
  static constexpr size_t kDefaultCapacity = 64;

  SnapshotPool(size_t capacity, size_t payload_capacity, size_t mask_bytes)
    : capacity_(capacity), slots_(new PoolSlot[capacity])
  {
    for(size_t i = 0; i < capacity_; i++)
    {
      slots_[i].snapshot.payload.reserve(payload_capacity);
      slots_[i].snapshot.active_mask.resize(mask_bytes);
    }
  }

  SnapshotPool(const SnapshotPool&) = delete;
  SnapshotPool& operator=(const SnapshotPool&) = delete;

  /**
   * @brief Find a free slot and take one reference on it.
   * Must be called from a single thread (the snapshot thread).
   * @return the slot, or nullptr (and exhausted() incremented) if all are in use.
   */
  PoolSlot* tryAcquire()
  {
    for(size_t n = 0; n < capacity_; n++)
    {
      if(++scan_from_ == capacity_)
      {
        scan_from_ = 0;
      }
      PoolSlot& slot = slots_[scan_from_];
      // acquire: synchronizes with the last release() by a consumer, so that
      // consumer's reads of the slot happen-before our next writes into it.
      if(slot.refs.load(std::memory_order_acquire) == 0)
      {
        slot.refs.store(1, std::memory_order_relaxed);
        return &slot;
      }
    }
    exhausted_.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }

  static void addRef(PoolSlot* slot)
  {
    slot->refs.fetch_add(1, std::memory_order_relaxed);
  }

  static void release(PoolSlot* slot)
  {
    slot->refs.fetch_sub(1, std::memory_order_release);
  }

  size_t capacity() const { return capacity_; }

  /// Number of slots with refs != 0. Diagnostic only; racy by nature.
  size_t inUse() const
  {
    size_t count = 0;
    for(size_t i = 0; i < capacity_; i++)
    {
      if(slots_[i].refs.load(std::memory_order_relaxed) != 0)
      {
        count++;
      }
    }
    return count;
  }

  uint64_t exhausted() const { return exhausted_.load(std::memory_order_relaxed); }

private:
  const size_t capacity_;
  std::unique_ptr<PoolSlot[]> slots_;
  size_t scan_from_ = 0;  // snapshot thread only
  std::atomic<uint64_t> exhausted_{ 0 };
};

}  // namespace DataTamer
