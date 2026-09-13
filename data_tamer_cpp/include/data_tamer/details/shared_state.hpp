#pragma once

#include "data_tamer/details/write_mutex.hpp"
#include "data_tamer/types.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <stdexcept>

namespace DataTamer
{

/**
 * @brief Append-only table of atomic words with stable addresses. The control
 * thread appends (one writer); any thread may read an existing index or size()
 * concurrently without locks: blocks are never moved or freed until
 * destruction, and each block pointer and the size are published with release
 * semantics after the new word has been written.
 *
 * Block i holds kFirstBlock << i words, so 26 blocks cover 2^31 series.
 */
class AtomicWordTable
{
public:
  static constexpr size_t kFirstBlock = 64;
  static constexpr size_t kBlocks = 26;

  AtomicWordTable() = default;
  AtomicWordTable(const AtomicWordTable&) = delete;
  AtomicWordTable& operator=(const AtomicWordTable&) = delete;
  ~AtomicWordTable()
  {
    for(auto& block : blocks_)
    {
      delete[] block.load(std::memory_order_relaxed);
    }
  }

  /// Number of words appended so far (acquire: their contents are visible).
  size_t size() const { return size_.load(std::memory_order_acquire); }

  /// Precondition: index < size() as observed by this thread.
  std::atomic<uint32_t>& operator[](size_t index) const
  {
    const auto [block, offset] = locate(index);
    return blocks_[block].load(std::memory_order_acquire)[offset];
  }

  /// Control thread only.
  void push_back(uint32_t value)
  {
    const size_t index = size_.load(std::memory_order_relaxed);
    const auto [block, offset] = locate(index);
    if(block >= kBlocks)
    {
      throw std::length_error("too many registered series");
    }
    auto* words = blocks_[block].load(std::memory_order_relaxed);
    if(!words)
    {
      words = new std::atomic<uint32_t>[kFirstBlock << block];
      blocks_[block].store(words, std::memory_order_release);
    }
    words[offset].store(value, std::memory_order_relaxed);
    size_.store(index + 1, std::memory_order_release);
  }

private:
  static std::pair<size_t, size_t> locate(size_t index)
  {
    size_t block = 0, base = 0, capacity = kFirstBlock;
    while(index >= base + capacity)
    {
      base += capacity;
      capacity <<= 1;
      ++block;
    }
    return { block, index - base };
  }

  std::array<std::atomic<std::atomic<uint32_t>*>, kBlocks> blocks_{};
  std::atomic<size_t> size_{ 0 };
};

/**
 * @brief State shared between a LogChannel and every LoggedValue registered
 * in it. Owned through shared_ptr by both, so writer-side operations
 * (set, setEnabled, transactions) never need the channel object and keep
 * working if the channel is destroyed first.
 *
 * Series flags are appended during registration (single control thread) and
 * read lock-free by the snapshot thread and by writers; AtomicWordTable keeps
 * them at stable addresses and makes appends safe to race with reads.
 */
class ChannelSharedState
{
public:
  /**
   * @brief Scoped ownership of this state's write mutex.
   *
   * A nested transaction for the same state is a no-op. The thread-local
   * linked chain is allocation-free and has no nesting-depth limit.
   * Transactions (and the LoggedValue guards built on them) belong to the
   * thread that created them: destroying one on another thread, or suspending
   * a coroutine while holding one, is undefined. Destruction out of creation
   * order on the same thread is supported: the node is unlinked and mutex
   * ownership passes to a younger transaction on the same state, if any.
   */
  class Transaction
  {
  public:
    explicit Transaction(ChannelSharedState& state) : state_(&state), previous_(active())
    {
      owns_ = !state.inTransactionOnThisThread();
      if(owns_)
      {
        state.write_mutex.lock();
      }
      active() = this;
    }

    ~Transaction()
    {
      if(active() == this)
      {
        active() = previous_;
      }
      else
      {
        // Out-of-order destruction: unlink this node; a younger transaction on
        // the same state (never owning, since we do) inherits the mutex.
        Transaction* heir = nullptr;
        for(auto* node = active(); node; node = node->previous_)
        {
          if(node->state_ == state_)
          {
            heir = node;  // younger than us: it was created after us
          }
          if(node->previous_ == this)
          {
            node->previous_ = previous_;
            break;
          }
        }
        if(owns_ && heir)
        {
          heir->owns_ = true;
          return;
        }
      }
      if(owns_)
      {
        state_->write_mutex.unlock();
      }
    }

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    Transaction(Transaction&&) = delete;
    Transaction& operator=(Transaction&&) = delete;

  private:
    friend class ChannelSharedState;

    static Transaction*& active()
    {
      static thread_local Transaction* transaction = nullptr;
      return transaction;
    }

    ChannelSharedState* state_;
    Transaction* previous_;
    bool owns_ = false;
  };

  /// The transaction lock: writers hold it for a scopedWrite(); the snapshot
  /// thread holds it for serialization. Priority-inheriting where available.
  WriteMutex write_mutex;

  /// SC publication pairs with the snapshot's SC load/exchange and reader epoch.
  std::atomic<bool> mask_dirty{ true };

  /// Controller only. Returns the generation of the new slot (always 1).
  uint32_t addSeries()
  {
    flags_.push_back(kFirstGeneration | kRegistered | kEnabled);
    return 1;
  }

  /// Generation currently occupying a slot (bumped on every re-registration).
  uint32_t generation(size_t index) const
  {
    return flags_[index].load(std::memory_order_seq_cst) >> kGenerationShift;
  }

  /// Whether `id` denotes the registration currently occupying its slot.
  bool isCurrent(const RegistrationID& id) const
  {
    return id.index_ < flags_.size() && generation(id.index_) == id.generation_;
  }

  size_t seriesCount() const { return flags_.size(); }

  bool isEnabled(size_t index) const
  {
    return (flags_[index].load(std::memory_order_seq_cst) & kFlagsMask) ==
           (kRegistered | kEnabled);
  }

  /// False for a stale or invalid id.
  bool isEnabled(const RegistrationID& id) const
  {
    if(id.index_ >= flags_.size())
    {
      return false;
    }
    const auto word = flags_[id.index_].load(std::memory_order_seq_cst);
    return (word >> kGenerationShift) == id.generation_ &&
           (word & kFlagsMask) == (kRegistered | kEnabled);
  }

  bool isRegistered(size_t index) const
  {
    return flags_[index].load(std::memory_order_seq_cst) & kRegistered;
  }

  /// Controller only: clear registration and wait for the reader before
  /// detaching the holder. The generation is kept, so the id stays valid for
  /// isEnabled() (false) until the slot is registered again.
  void setUnregistered(size_t index)
  {
    flags_[index].fetch_and(~uint32_t(kRegistered), std::memory_order_seq_cst);
    mask_dirty.store(true, std::memory_order_seq_cst);
  }

  /// Highest generation a slot can reach (24 bits); see canReregister().
  static constexpr uint32_t kMaxGeneration = (uint32_t(1) << 24) - 1;

  /// Controller only: true if the slot can still take a replacement (its
  /// generation counter is not exhausted). Check before touching the holder.
  bool canReregister(size_t index) const { return generation(index) < kMaxGeneration; }

  /// Controller only: publish a replacement registration in a slot whose holder
  /// is already initialized. Returns the new generation; older ids are stale.
  /// Precondition: canReregister(index).
  uint32_t setReregistered(size_t index)
  {
    const auto previous = flags_[index].load(std::memory_order_seq_cst);
    const uint32_t generation = (previous >> kGenerationShift) + 1;
    if(generation > kMaxGeneration)
    {
      throw std::length_error("registration slot exhausted: it was re-registered "
                              "16 million times");
    }
    flags_[index].store((generation << kGenerationShift) | kRegistered | kEnabled,
                        std::memory_order_seq_cst);
    mask_dirty.store(true, std::memory_order_seq_cst);
    return generation;
  }

  /// Lock-free; changes requested enablement only, never registration liveness.
  void setEnabled(size_t index, bool enable)
  {
    const auto old =
        enable ? flags_[index].fetch_or(kEnabled, std::memory_order_seq_cst) :
                 flags_[index].fetch_and(~uint32_t(kEnabled), std::memory_order_seq_cst);
    if(bool(old & kEnabled) != enable)
    {
      mask_dirty.store(true, std::memory_order_seq_cst);
    }
  }

  /// Lock-free and noexcept. Returns false, changing nothing, if `id` is stale
  /// or invalid: the generation check and the flag update are one atomic
  /// exchange, so a concurrent re-registration cannot slip in between.
  bool setEnabled(const RegistrationID& id, bool enable) noexcept
  {
    if(id.index_ >= flags_.size())
    {
      return false;
    }
    auto& word = flags_[id.index_];
    auto current = word.load(std::memory_order_seq_cst);
    for(;;)
    {
      if((current >> kGenerationShift) != id.generation_)
      {
        return false;
      }
      const uint32_t desired =
          enable ? (current | kEnabled) : (current & ~uint32_t(kEnabled));
      if(desired == current)
      {
        return true;
      }
      if(word.compare_exchange_weak(current, desired, std::memory_order_seq_cst))
      {
        mask_dirty.store(true, std::memory_order_seq_cst);
        return true;
      }
    }
  }

  [[nodiscard]] bool inTransactionOnThisThread() const noexcept
  {
    for(auto* transaction = Transaction::active(); transaction;
        transaction = transaction->previous_)
    {
      if(transaction->state_ == this)
      {
        return true;
      }
    }
    return false;
  }

private:
  // One word per series: low byte holds the flags, the rest the generation.
  static constexpr uint32_t kRegistered = 1;
  static constexpr uint32_t kEnabled = 2;
  static constexpr uint32_t kFlagsMask = 0xFF;
  static constexpr unsigned kGenerationShift = 8;
  static constexpr uint32_t kFirstGeneration = uint32_t(1) << kGenerationShift;
  static_assert(std::atomic<uint32_t>::is_always_lock_free, "series flags must be "
                                                            "lock-free");
  AtomicWordTable flags_;
};

}  // namespace DataTamer
