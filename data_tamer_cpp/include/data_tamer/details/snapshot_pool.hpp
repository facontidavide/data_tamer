#pragma once

#include "data_tamer/data_sink.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace DataTamer
{

/// One pre-allocated snapshot plus its reference count (0 means free). Only the
/// snapshot thread moves refs from 0 to 1 and writes the snapshot, while it holds the
/// only reference; sinks read it and release. refs has its own cache line, so sink
/// traffic does not touch the snapshot data.
struct PoolSlot
{
  Snapshot snapshot;
  alignas(64) std::atomic<uint32_t> refs{ 0 };
};

/**
 * @brief Fixed-size pool of PoolSlot, shared by its LogChannel and every SnapshotRef
 * handed to a sink. Allocates only in the constructor.
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
   * @brief Takes a free slot with one reference. Snapshot thread only; real-time safe.
   * @return the slot, or nullptr (counted in exhausted()) if every slot is in use.
   */
  [[nodiscard]] PoolSlot* tryAcquire()
  {
    for(size_t n = 0; n < capacity_; n++)
    {
      if(++scan_from_ == capacity_)
      {
        scan_from_ = 0;
      }
      PoolSlot& slot = slots_[scan_from_];
      // acquire: pairs with release(), so a sink's reads of the slot precede our writes.
      if(slot.refs.load(std::memory_order_acquire) == 0)
      {
        slot.refs.store(1, std::memory_order_relaxed);
        return &slot;
      }
    }
    exhausted_.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }

  /// Wraps a reference already taken on `slot` in a SnapshotRef. Library internal.
  [[nodiscard]] static SnapshotRef adopt(std::shared_ptr<SnapshotPool> pool,
                                         PoolSlot* slot)
  {
    return SnapshotRef(std::move(pool), slot);
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

  /// Number of slots in use. Diagnostic only: the count is racy.
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
