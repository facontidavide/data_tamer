#pragma once

#include "data_tamer/types.hpp"
#include "data_tamer/details/locked_reference.hpp"
#include "data_tamer/details/shared_state.hpp"

#include <atomic>
#include <memory>
#include <type_traits>

namespace DataTamer
{

class LogChannel;

namespace details
{
// std::atomic<T> hard-fails for a T that is not trivially copyable, even inside a
// short-circuited "&&". So the checks that rule T out run first, as a defaulted
// non-type parameter, and std::atomic<T> is only named for a T that qualifies.
template <typename T,
          bool = std::is_trivially_copyable_v<T> && IsNumericType<T>() && sizeof(T) <= 8>
struct is_atomic_scalar : std::false_type
{
};

template <typename T>
struct is_atomic_scalar<T, true> : std::bool_constant<std::atomic<T>::is_always_lock_free>
{
};
}  // namespace details

/// True for the scalars (numbers, bool, char, enums) of at most 8 bytes that have a
/// lock-free std::atomic. LoggedValue stores them atomically: set() and get() are
/// single relaxed stores and loads.
template <typename T>
inline constexpr bool is_atomic_scalar_v = details::is_atomic_scalar<T>::value;

/**
 * @brief A variable that registers itself in a LogChannel when created and
 * unregisters when destroyed. Create it with LogChannel::createLoggedValue(). Its
 * member functions are defined in channel.hpp: include that to use a LoggedValue.
 *
 * Scalars (see is_atomic_scalar_v) are stored in a std::atomic: set() and get() are
 * wait-free. Other types are accessed under the channel's write mutex, like
 * LogChannel::scopedWrite(), so they block while a snapshot serializes.
 *
 * A lone set() is never torn. Values that must be captured together belong in one
 * struct, or inside a LogChannel::scopedWrite() transaction.
 *
 * The destructor unregisters, which waits for a snapshot in progress: never release
 * the last shared_ptr on a real-time thread or inside scopedWrite().
 */
template <typename T>
class LoggedValue
{
protected:
  LoggedValue(const std::shared_ptr<LogChannel>& channel, const std::string& name,
              T initial_value);

  friend LogChannel;

public:
  static constexpr bool kAtomic = is_atomic_scalar_v<T>;
  using Storage = std::conditional_t<kAtomic, std::atomic<T>, T>;

  ~LoggedValue();

  LoggedValue(LoggedValue const& other) = delete;
  LoggedValue& operator=(LoggedValue const& other) = delete;

  // The channel holds a pointer to value_; moving would leave it dangling.
  LoggedValue(LoggedValue&& other) = delete;
  LoggedValue& operator=(LoggedValue&& other) = delete;

  /// Stores the value. Wait-free for scalars; other types lock the channel's write
  /// mutex (no-op inside scopedWrite() on the same thread). Does not re-enable a
  /// value disabled with setEnabled(false).
  void set(const T& value);

  /// Returns a copy of the value. Locks like set().
  [[nodiscard]] T get() const;

  /**
   * Read/write access to a non-scalar value (scalars: use set() and get()). The
   * guard holds the channel's write mutex while it lives, and nests inside
   * scopedWrite(). The snapshot thread waits for it: keep the scope short and free
   * of blocking calls.
   */
  [[nodiscard]] MutablePtr<T> getMutablePtr();

  /// Read-only counterpart of getMutablePtr(), for non-scalar values.
  [[nodiscard]] ConstPtr<T> getConstPtr() const;

  /// Includes or excludes the value from the snapshots. Lock-free; callable from any
  /// thread, even after the channel is destroyed.
  void setEnabled(bool enabled) noexcept;

  [[nodiscard]] bool isEnabled() const noexcept;

private:
  std::shared_ptr<ChannelSharedState> state_;
  std::weak_ptr<LogChannel> channel_;  // destructor only
  Storage value_;
  RegistrationID id_;
};

}  // namespace DataTamer
