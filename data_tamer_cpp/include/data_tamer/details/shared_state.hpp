#pragma once

#include "data_tamer/details/write_mutex.hpp"
#include "data_tamer/types.hpp"

#include <atomic>
#include <cstddef>
#include <deque>

namespace DataTamer
{

/**
 * @brief State shared between a LogChannel and every LoggedValue registered
 * in it. Owned through shared_ptr by both, so writer-side operations
 * (set, setEnabled, transactions) never need the channel object and keep
 * working if the channel is destroyed first.
 *
 * Series flags are appended during registration (setup only, single
 * control thread) and read by the snapshot thread; std::deque keeps the
 * atomics at stable addresses while appending.
 */
class ChannelSharedState
{
public:
  /**
   * @brief Scoped ownership of this state's write mutex.
   *
   * A nested transaction for the same state is a no-op. The thread-local
   * linked chain is allocation-free and has no nesting-depth limit.
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
      active() = previous_;
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

  void addSeries() { flags_.emplace_back(kRegistered | kEnabled); }

  size_t seriesCount() const { return flags_.size(); }

  bool isEnabled(size_t index) const
  {
    return flags_[index].load(std::memory_order_seq_cst) == (kRegistered | kEnabled);
  }

  bool isRegistered(size_t index) const
  {
    return flags_[index].load(std::memory_order_seq_cst) & kRegistered;
  }

  /// Controller only: initialize the holder before publishing registration;
  /// clear registration and wait for the reader before detaching it.
  void setRegistered(size_t index, bool registered)
  {
    if(registered)
      flags_[index].store(kRegistered | kEnabled, std::memory_order_seq_cst);
    else
      flags_[index].fetch_and(uint8_t(~kRegistered), std::memory_order_seq_cst);
    mask_dirty.store(true, std::memory_order_seq_cst);
  }

  /// Lock-free; changes requested enablement only, never registration liveness.
  void setEnabled(size_t index, bool enable)
  {
    const auto old =
        enable ? flags_[index].fetch_or(kEnabled, std::memory_order_seq_cst) :
                 flags_[index].fetch_and(uint8_t(~kEnabled), std::memory_order_seq_cst);
    if(bool(old & kEnabled) != enable)
      mask_dirty.store(true, std::memory_order_seq_cst);
  }

  void setEnabled(const RegistrationID& id, bool enable)
  {
    for(size_t i = 0; i < id.fields_count; i++)
      setEnabled(id.first_index + i, enable);
  }

  void setRegistered(const RegistrationID& id, bool registered)
  {
    for(size_t i = 0; i < id.fields_count; i++)
      setRegistered(id.first_index + i, registered);
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
  static constexpr uint8_t kRegistered = 1;
  static constexpr uint8_t kEnabled = 2;
  static_assert(std::atomic<uint8_t>::is_always_lock_free, "series flags must be "
                                                           "lock-free");
  std::deque<std::atomic<uint8_t>> flags_;
};

}  // namespace DataTamer
