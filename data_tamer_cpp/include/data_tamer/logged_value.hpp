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
// std::atomic<T>'s primary template hard-fails (static_assert) for a T that
// isn't trivially copyable, even inside a short-circuited "&&" — the class
// still has to be instantiated to name is_always_lock_free. So the checks
// that would rule T out are done first, as a non-type template parameter
// selecting between two definitions, and std::atomic<T> is only named in the
// branch where T already qualifies.
template <typename T, bool = std::is_trivially_copyable_v<T> && IsNumericType<T>() &&
                                  sizeof(T) <= 8>
struct is_atomic_scalar : std::false_type
{
};

template <typename T>
struct is_atomic_scalar<T, true> : std::bool_constant<std::atomic<T>::is_always_lock_free>
{
};
}  // namespace details

/// Scalars small enough for a lock-free std::atomic: stored atomically, so
/// set()/get() are single relaxed stores/loads.
template <typename T>
inline constexpr bool is_atomic_scalar_v = details::is_atomic_scalar<T>::value;

/**
 * @brief The LoggedValue class is a container of a variable that
 * automatically register/unregister to a Channel when created/destroyed.
 *
 * Scalars (arithmetic types, bool, char, small enums) are stored in a
 * std::atomic: set() and get() are wait-free and never take a lock. Other
 * types are written under the channel's transaction mutex (see
 * LogChannel::scopedWrite()).
 *
 * Consistency between several values is opt-in: a lone set() promises only
 * that the value itself is never torn. Values that must be captured together
 * belong in one struct, or inside a LogChannel::scopedWrite() transaction.
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
  /// Return type of the (deprecated) getLockedPtr(); getMutablePtr() itself
  /// returns the concrete AtomicProxy<T>/MutablePtr<T> directly, so this
  /// alias only still exists for that one caller.
  using MutableProxy = std::conditional_t<kAtomic, AtomicProxy<T>, MutablePtr<T>>;

  ~LoggedValue();

  LoggedValue(LoggedValue const& other) = delete;
  LoggedValue& operator=(LoggedValue const& other) = delete;

  // The channel holds a pointer to value_; moving would leave it dangling.
  LoggedValue(LoggedValue&& other) = delete;
  LoggedValue& operator=(LoggedValue&& other) = delete;

  /**
   * @brief set the value of the variable. Wait-free for scalars; takes the
   * channel's transaction mutex for other types (unless the calling thread
   * already holds it through scopedWrite()).
   *
   * @param value        new value
   * @param auto_enable  if true and the value is disabled, enable it
   */
  void set(const T& value, bool auto_enable = true);

  /// @brief get the stored value (a copy).
  [[nodiscard]] T get() const;

  [[deprecated("use getMutablePtr() instead")]] [[nodiscard]] MutableProxy getLockedPtr()
  {
    return getMutablePtr();
  }

  /**
   * Read/write access. For scalars this is a write-back proxy: edits become
   * visible when the proxy is destroyed, and two overlapping proxies are
   * last-writer-wins — prefer set(). For other types the proxy holds the
   * transaction mutex for its lifetime, blocking the snapshot thread: keep it
   * short and allocation-free.
   */
  template <typename U = T, std::enable_if_t<is_atomic_scalar_v<U>, bool> = true>
  [[deprecated("for scalar values use set()/get(); the returned proxy writes back on destruction")]]
  [[nodiscard]] AtomicProxy<T> getMutablePtr();

  template <typename U = T, std::enable_if_t<!is_atomic_scalar_v<U>, bool> = true>
  [[nodiscard]] MutablePtr<T> getMutablePtr();

  /// Read-only access. For scalars: a copy taken now. For other types: the
  /// transaction mutex is held for the proxy's lifetime.
  template <typename U = T, std::enable_if_t<is_atomic_scalar_v<U>, bool> = true>
  [[deprecated("for scalar values use get(); the returned proxy holds a copy")]]
  [[nodiscard]] AtomicConstProxy<T> getConstPtr();

  template <typename U = T, std::enable_if_t<!is_atomic_scalar_v<U>, bool> = true>
  [[nodiscard]] ConstPtr<T> getConstPtr();

  /// @brief Disabling a LoggedValue means that we will not record it in the snapshot.
  /// Wait-free; callable from any thread, even after the channel is destroyed.
  void setEnabled(bool enabled);

  [[nodiscard]] bool isEnabled() const;

private:
  std::shared_ptr<ChannelSharedState> state_;
  std::weak_ptr<LogChannel> channel_;  // destructor only
  Storage value_;
  RegistrationID id_;
};

}  // namespace DataTamer
