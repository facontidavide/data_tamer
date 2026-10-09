#pragma once

#include <atomic>
#include <cstdint>

namespace DataTamer::details
{
/**
 * @brief Counts the threads that wait on an atomic, so that the thread that changes it
 * calls notify_all() only when one of them waits. libstdc++ routes notify_all() through
 * a waiter table that all the atomics of the process share, and makes a futex call
 * whenever any thread waits on an atomic of the same bucket: without the count, a wait
 * elsewhere in the process would cost the real-time path a system call.
 *
 * The waiter registers (Scope) before it reads the atomic, and the notifier changes the
 * atomic before it reads the count, all seq_cst: either the waiter reads the change, or
 * the notifier sees the waiter and notifies it.
 */
class WaiterCount
{
public:
  /// Registers a waiter for its lifetime.
  class Scope
  {
  public:
    explicit Scope(WaiterCount& waiters) : count_(waiters.count_)
    {
      count_.fetch_add(1, std::memory_order_seq_cst);
    }
    ~Scope() { count_.fetch_sub(1, std::memory_order_seq_cst); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

  private:
    std::atomic<uint32_t>& count_;
  };

  /// Call after the change of `changed` that the waiters wait for.
  template <typename T>
  void notifyIfWaiting(std::atomic<T>& changed) const
  {
    if(count_.load(std::memory_order_seq_cst) != 0)
    {
      changed.notify_all();
    }
  }

private:
  std::atomic<uint32_t> count_{ 0 };
};
}  // namespace DataTamer::details
