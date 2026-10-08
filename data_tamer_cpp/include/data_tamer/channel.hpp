#pragma once

#include "data_tamer/fwd.hpp"
#include "data_tamer/contrib/SerializeMe.hpp"
#include "data_tamer/custom_types.hpp"
#include "data_tamer/data_sink.hpp"
#include "data_tamer/logged_value.hpp"
#include "data_tamer/names.hpp"
#include "data_tamer/types.hpp"
#include "data_tamer/values.hpp"
#include "data_tamer/details/abi.hpp"
#include "data_tamer/details/shared_state.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace DataTamer
{

/// System-clock time since the epoch: the default timestamp of a snapshot.
inline std::chrono::nanoseconds NsecSinceEpoch()
{
  auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch);
}

/// Returned by LogChannel::scopedWrite(): holds the channel's write mutex for its
/// scope. A nested transaction on the same thread does nothing.
using WriteTransaction = ChannelSharedState::Transaction;

//---------------------------------------------------------

/// Outcome of LogChannel::takeSnapshot() and tryTakeSnapshot().
enum class SnapshotResult : uint8_t
{
  /// Captured and queued on every attached sink.
  ok,
  /// Captured, but some sinks refused it because their SinkWorker is stopped
  /// (Stats::dropped_by_sink tells which).
  partial,
  /// Captured, but every attached sink refused it (every SinkWorker is stopped).
  rejected,
  /// No sink is attached: nothing captured. Before startLogging() the schema stays
  /// open.
  no_sinks,
  /// tryTakeSnapshot() before startLogging(): nothing captured.
  not_started,
  /// Sinks still hold every pool slot, so the sample is lost (see poolExhausted()).
  pool_exhausted,
  /// tryTakeSnapshot(): the payload outgrew the slot (see droppedOversize()).
  oversize,
  /// tryTakeSnapshot(): a writer held the write mutex past the spin budget. Both
  /// variants: the calling thread holds it itself (scopedWrite(), a LoggedValue guard).
  blocked
};

/**
 * @brief Records the values registered in it as one snapshot per takeSnapshot() call
 * and hands each snapshot to its sinks. Get channels from
 * ChannelsRegistry::getChannel(); use one channel per rate or logical group.
 *
 * Register every value (registerValue(), createLoggedValue()) before startLogging(),
 * which the first takeSnapshot() with a sink attached calls. Several threads may take
 * snapshots; they take turns on the write mutex. Registration, unregister(), sink
 * changes and startLogging() are control operations that can block, some until a
 * snapshot in progress ends: never call them from a serializer or a sink callback.
 */
class LogChannel : public std::enable_shared_from_this<LogChannel>
{
protected:
  // Use create(): the channel must be owned by a std::shared_ptr, because
  // LoggedValue keeps a std::weak_ptr to it.
  LogChannel(std::string name);

public:
  /// Creates a channel. Prefer ChannelsRegistry::getChannel(), which also applies the
  /// registry's defaults and default sinks. The name is a line of the schema text: it
  /// can hold spaces, but std::runtime_error is thrown if it is empty or holds a
  /// control character.
  [[nodiscard]] static std::shared_ptr<LogChannel> create(std::string name);

  ~LogChannel();

  LogChannel(const LogChannel&) = delete;
  LogChannel& operator=(const LogChannel&) = delete;

  LogChannel(LogChannel&&) = delete;
  LogChannel& operator=(LogChannel&&) = delete;

  /**
   * @brief Registers a variable to be recorded in every snapshot.
   *
   * The channel borrows the pointer: it must stay valid until unregister() has
   * returned or the channel is destroyed. Write the variable inside scopedWrite()
   * whenever another thread may take a snapshot meanwhile.
   *
   * The name must be unique in the channel, non-empty and free of whitespace and
   * control characters, and so must the name of a custom type and of each of its
   * fields; JoinNames() builds hierarchical names. Throws std::runtime_error for a
   * duplicate or invalid name, and for a new name once logging started. A call that
   * throws leaves the channel unchanged. Not real-time safe.
   *
   * @return the ID for unregister(), setEnabled() and isEnabled()
   */
  template <typename T, bool = true>
  RegistrationID registerValue(const std::string& name, const T* value);

  /**
   * @brief registerValue() for an atomic scalar: read with a relaxed load at each
   * snapshot and recorded like T. Any thread can write it; scopedWrite() is only
   * needed to keep it consistent with other values.
   */
  template <typename T, std::enable_if_t<IsNumericType<T>(), bool> = true>
  RegistrationID registerValue(const std::string& name, const std::atomic<T>* value);

  /**
   * @brief registerValue() for a std::vector (or similar container) of numbers or
   * custom types. Its size and elements are read at each snapshot, so it may grow
   * between snapshots (see setPayloadCapacity()).
   */
  template <
      template <class, class> class Container, class T, class... TArgs,
      std::enable_if_t<!has_TypeDefinition<Container<T, TArgs...>>::value, bool> = true>
  RegistrationID registerValue(const std::string& name,
                               const Container<T, TArgs...>* value);

  /// registerValue() for a fixed-size std::array of numbers or custom types.
  template <typename T, size_t N,
            std::enable_if_t<!has_TypeDefinition<std::array<T, N>>::value, bool> = true>
  RegistrationID registerValue(const std::string& name, const std::array<T, N>* value);

  /**
   * @brief Advanced: registers a value that your own CustomSerializer serializes,
   * bypassing the built-in serialization. Generic decoders may not be able to read
   * the result: prefer TypeDefinitionTrait<T> (or TypeDefinition()) when you can.
   *
   * The name, the pointer and `type_info->typeName()` follow the rules of
   * registerValue(). Throws std::invalid_argument if `type_info` is null.
   */
  template <typename T>
  RegistrationID registerCustomValue(const std::string& name, const T* value,
                                     CustomSerializer::Ptr type_info);

  /**
   * @brief Creates a variable that registers `name` now and unregisters it when the
   * returned LoggedValue is destroyed. Name rules, exceptions and the guarantee of a
   * throwing call as registerValue(); see LoggedValue for the threading rules.
   */
  template <typename T = double>
  [[nodiscard]] std::shared_ptr<LoggedValue<T>> createLoggedValue(std::string const& name,
                                                                  T initial_value = T{});

  /// Name of this channel.
  [[nodiscard]] const std::string& channelName() const;

  /**
   * @brief Includes or excludes a value from the snapshots. Much cheaper than
   * unregister(), and allowed after logging started. Callable from any thread,
   * lock-free and allocation-free. Throws std::invalid_argument for a stale or
   * invalid id: on a real-time thread use trySetEnabled().
   */
  void setEnabled(const RegistrationID& id, bool enable);

  /// Like setEnabled(), but returns false instead of throwing for a stale or invalid
  /// id. noexcept, lock-free and allocation-free: safe on real-time threads.
  [[nodiscard]] bool trySetEnabled(const RegistrationID& id, bool enable) noexcept;

  /// True if the value is registered and enabled; false after unregister() and for a
  /// stale or invalid id. Lock-free.
  [[nodiscard]] bool isEnabled(const RegistrationID& id) const noexcept;

  /// Stops recording the value; the pointer may be freed once this returns. The value
  /// stays in the schema (as disabled), and its name can be registered again with the
  /// same type. Waits for a snapshot in progress: not real-time safe. Throws
  /// std::invalid_argument for a stale or invalid id.
  void unregister(const RegistrationID& id);

  /**
   * @brief Attaches a sink (a SinkWorker, see MCAPSink::create() or
   * SinkWorker::create<T>()) that receives this channel's snapshots.
   *
   * A channel holds at most kMaxSinks sinks: one more throws std::runtime_error.
   * If logging started, this allocates the sink's queue and calls its onSchema() on
   * the calling thread: not real-time safe. Otherwise startLogging() does both.
   * @return false if the sink was attached already, true otherwise.
   */
  bool addDataSink(std::shared_ptr<SinkWorker> sink);

  /**
   * @brief Detaches a sink. Snapshots already queued on it are still delivered.
   * Does nothing if the sink is not attached. Waits for a snapshot in progress: not
   * real-time safe.
   */
  void removeDataSink(std::shared_ptr<SinkWorker> sink);

  /// Number of attached sinks.
  [[nodiscard]] size_t getNumberOfSinks() const;

  /**
   * @brief Freezes the schema, allocates the snapshot pool and sends the schema to
   * the sinks (onSchema(), on the calling thread).
   *
   * Call it once after registering all values and before the snapshot loop. It
   * allocates and runs sink callbacks: not real-time safe. Does nothing if logging
   * started already. Throws if a sink's onSchema() throws or an allocation fails;
   * calling it again retries.
   */
  void startLogging();

  /// True once startLogging() has completed.
  [[nodiscard]] bool isLoggingStarted() const;

  /**
   * @brief Serializes the enabled values and queues the snapshot on every sink.
   *
   * Several threads may take snapshots of one channel: they take turns on its write
   * mutex. The first call with a sink attached calls startLogging(). It can block on a
   * writer and allocate: on a real-time thread use tryTakeSnapshot().
   * @param timestamp time of the snapshot; by default the system clock.
   */
  [[nodiscard]] SnapshotResult
  takeSnapshot(std::chrono::nanoseconds timestamp = NsecSinceEpoch());

  /**
   * @brief Lock-free takeSnapshot() for real-time threads: it never blocks or
   * allocates. Where takeSnapshot() would wait or allocate, it returns `blocked` (a
   * writer or another snapshot holds the mutex), `oversize` (the payload outgrew its
   * slot) or `not_started` (call startLogging() before the loop). Serializers run on
   * this path: they must not throw, allocate or block.
   */
  [[nodiscard]] SnapshotResult
  tryTakeSnapshot(std::chrono::nanoseconds timestamp = NsecSinceEpoch());

  /// A copy of the channel's schema. Takes the control mutex: not real-time safe.
  [[nodiscard]] Schema getSchema() const;

  /**
   * @brief Holds the channel's write mutex for the scope of the returned object, so
   * that a group of writes appears in one snapshot or in none:
   * `auto tx = channel->scopedWrite();` then write.
   *
   * Use it when a thread other than the snapshot thread must keep several values
   * consistent with each other (a position and the velocity computed from it); a lone
   * write is only captured untorn. It is also the only correct way for such a thread
   * to write a variable registered with registerValue(): the snapshot thread reads it
   * without a lock of its own.
   *
   * Keep the scope short and free of blocking calls: the snapshot thread waits for it
   * (takeSnapshot() blocks, tryTakeSnapshot() returns `blocked`). Nested transactions
   * and LoggedValue guards on the same thread are safe; a snapshot that thread takes
   * meanwhile returns `blocked`.
   */
  [[nodiscard]] WriteTransaction scopedWrite();

  /// Snapshots delayed by a writer past the spin budget: takeSnapshot() blocked,
  /// tryTakeSnapshot() returned `blocked`.
  [[nodiscard]] uint64_t writeLockContended() const;

  /// Longest time takeSnapshot() blocked on the write mutex, in nanoseconds.
  [[nodiscard]] uint64_t writeLockWaitMaxNs() const;

  /// Snapshot attempts that found no free pool slot.
  [[nodiscard]] uint64_t poolExhausted() const;

  /// Sets the minimum payload bytes reserved by each pool slot. Zero (the default)
  /// sizes the slots from the registered values at startLogging(); raise it if
  /// vectors grow at run time, or tryTakeSnapshot() returns `oversize`. Call it
  /// before startLogging(): it throws std::runtime_error afterwards.
  void setPayloadCapacity(size_t bytes);

  /// Sets the number of pool slots: how many snapshots sinks may hold or have queued
  /// at the same time (default 64). Each attached sink gets a queue of the same size.
  /// Call it before startLogging(): it throws std::runtime_error afterwards, and
  /// std::invalid_argument for zero.
  void setPoolCapacity(size_t count);

  /// Sets the pool size in time: ceil(stall_tolerance / snapshot_period) slots, enough
  /// to absorb a sink stall of that length (200 ms at 1 kHz gives 200 slots). Both
  /// arguments must be positive (std::invalid_argument); otherwise as the count
  /// overload.
  void setPoolCapacity(std::chrono::nanoseconds stall_tolerance,
                       std::chrono::nanoseconds snapshot_period);

  /// Slots that takeSnapshot() grew because the payload outgrew them.
  [[nodiscard]] uint64_t payloadReallocations() const;

  /// tryTakeSnapshot() calls that returned `oversize`.
  [[nodiscard]] uint64_t droppedOversize() const;

  /// Calls of takeSnapshot() and tryTakeSnapshot() so far, whatever the result
  /// (calls that threw count too).
  [[nodiscard]] uint64_t snapshotAttempts() const;

  /// Snapshots that at least one sink took: the results `ok` and `partial`. Queued is
  /// not delivered: SinkWorker::delivered() counts deliveries. Read it before
  /// snapshotAttempts() so that it never exceeds them (stats() does).
  [[nodiscard]] uint64_t snapshotsAccepted() const;

  /// The most sinks a channel holds.
  static constexpr size_t kMaxSinks = 8;

  /// Building block of Stats::dropped_by_sink: prefer stats(). Writes up to `capacity`
  /// attached sinks to `sinks` and, to `dropped`, the publications each refused because
  /// its SinkWorker is stopped. Returns the number of attached sinks, which may exceed
  /// `capacity`. The order is unspecified, and the pointers only tell sinks apart (a
  /// removed worker's address can be reused): do not dereference them. Takes the
  /// control mutex: not real-time safe.
  [[nodiscard]] size_t sinkDropped(const SinkWorker** sinks, uint64_t* dropped,
                                   size_t capacity) const;

  /// Deprecated: read Stats::dropped_by_sink. Publications this sink refused; zero if
  /// it is not attached.
  [[deprecated("read stats().dropped_by_sink")]] [[nodiscard]] uint64_t
  droppedSnapshots(const std::shared_ptr<SinkWorker>& sink) const;

  /// Publications one attached sink refused. See sinkDropped().
  struct SinkDrops
  {
    const SinkWorker* sink = nullptr;
    uint64_t dropped = 0;
  };

  /// The channel's counters, read one by one rather than at one instant: only
  /// accepted <= attempts is guaranteed. Not trivially copyable (holds a vector).
  struct Stats
  {
    uint64_t write_lock_contended = 0;
    uint64_t write_lock_wait_max_ns = 0;
    uint64_t pool_exhausted = 0;
    uint64_t payload_reallocations = 0;
    uint64_t dropped_oversize = 0;
    /// Calls of takeSnapshot()/tryTakeSnapshot(); see snapshotAttempts().
    uint64_t attempts = 0;
    /// Snapshots at least one sink took (ok + partial); see snapshotsAccepted().
    uint64_t accepted = 0;
    /// One entry per sink attached at the moment of the read, in no promised
    /// order; see sinkDropped().
    std::vector<SinkDrops> dropped_by_sink;
  };

  /// Reads every counter. Takes the control mutex and allocates: not for a real-time
  /// thread. A difference between two results, or `attempts - accepted` over a window,
  /// is approximate by the calls in flight: clamp it at zero. Inline on purpose: see
  /// DATA_TAMER_INLINE_LOCAL (details/abi.hpp).
  [[nodiscard]] DATA_TAMER_INLINE_LOCAL Stats stats() const
  {
    Stats result;
    result.write_lock_contended = writeLockContended();
    result.write_lock_wait_max_ns = writeLockWaitMaxNs();
    result.pool_exhausted = poolExhausted();
    result.payload_reallocations = payloadReallocations();
    result.dropped_oversize = droppedOversize();
    result.accepted = snapshotsAccepted();
    result.attempts = snapshotAttempts();
    const SinkWorker* sinks[kMaxSinks];
    uint64_t dropped[kMaxSinks];
    const size_t count = sinkDropped(sinks, dropped, kMaxSinks);
    for(size_t i = 0; i < count && i < kMaxSinks; ++i)
    {
      result.dropped_by_sink.push_back({ sinks[i], dropped[i] });
    }
    return result;
  }

private:
  template <typename T>
  friend class LoggedValue;
  friend class ChannelsRegistry;
  /// The attached sinks, for ChannelsRegistry::stopAll().
  [[nodiscard]] std::vector<std::shared_ptr<SinkWorker>> dataSinks() const;
  /// State shared with this channel's LoggedValues (enable flags, write mutex).
  [[nodiscard]] std::shared_ptr<ChannelSharedState> sharedState() const;

  struct Pimpl;
  SnapshotResult takeSnapshotImpl(std::chrono::nanoseconds timestamp, bool real_time);
  std::unique_ptr<Pimpl> _p;

  /// The channel's custom-type serializers. Control path only.
  TypesRegistry& typeRegistry();

  std::mutex& controlMutex();
  bool schemaFrozen() const;
  bool hasCustomType(const std::string& type_name) const;

  /// The custom types a registration adds to the schema, by type name.
  using PendingTypes = std::map<std::string, FieldsVector>;

  /// Adds to `types` the custom types `T` needs that the schema lacks, without changing
  /// the channel. Throws if the schema is frozen and `T` is not in it.
  template <typename T>
  void discoverTypes(PendingTypes& types);

  template <typename T>
  void discoverTypesImpl(PendingTypes& types, FieldsVector& fields, const char* name);

  /// Used by the registerValue() templates of binaries built against older headers.
  void addCustomType(const std::string& custom_type_name, const FieldsVector& fields);

  /// Throws std::runtime_error if `name` is empty or contains whitespace or control
  /// characters. Called before type discovery, so that a rejected name leaves the
  /// channel unchanged.
  void checkValueName(const std::string& name) const;

  /// Registers a value that needs no new custom type.
  [[nodiscard]] RegistrationID registerValueImpl(const std::string& name,
                                                 ValuePtr&& value_ptr,
                                                 CustomSerializer::Ptr type_info);

  /// Registers the value and adds `types` to the schema as one step: it completes, or
  /// the channel is unchanged.
  [[nodiscard]] RegistrationID registerValueWithTypes(const std::string& name,
                                                      ValuePtr&& value_ptr,
                                                      CustomSerializer::Ptr type_info,
                                                      PendingTypes&& types);
};

//----------------------------------------------------------------------
//----------------------------------------------------------------------
//----------------------------------------------------------------------

template <typename T>
void LogChannel::discoverTypesImpl(PendingTypes& types, FieldsVector& fields,
                                   const char* field_name)
{
  using SerializeMe::container_info;
  // A container member is described by its element type ("float64[3] axis", "Pose[]
  // poses"); any other member's value_type is its own type.
  using Type = typename container_info<T>::value_type;
  TypeField field;
  field.field_name = field_name;
  if constexpr(container_info<T>::is_container)
  {
    field.is_vector = true;
    field.array_size = container_info<T>::size;
  }
  field.type = GetBasicType<Type>();
  if constexpr(GetBasicType<Type>() == BasicType::OTHER)
  {
    field.type_name = CustomTypeName<Type>::get();
    discoverTypes<Type>(types);
  }
  else
  {
    field.type_name = ToStr(field.type);
  }
  fields.push_back(field);
}

template <typename T>
inline void LogChannel::discoverTypes(PendingTypes& types)
{
  if constexpr(!IsNumericType<
                   T>())  // everything below must not be instantiated for numbers
  {
    using namespace SerializeMe;
    static_assert(has_TypeDefinition<T>(), "Missing TypeDefinition");

    const std::string type_name(CustomTypeName<T>::get());
    if(schemaFrozen() && !hasCustomType(type_name))
    {
      throw std::runtime_error("channel '" + channelName() +
                               "': can't add custom type '" + type_name +
                               "' after recording started");
    }
    // Throws if another C++ type uses this name.
    (void)typeRegistry().getSerializer<T>();
    if(schemaFrozen() || hasCustomType(type_name))
    {
      return;
    }
    // Entered before its fields are visited: a type that contains itself finds the entry.
    const auto [entry, added] = types.try_emplace(type_name);
    if(!added)
    {
      return;
    }
    FieldsVector fields;
    auto func = [this, &types, &fields](const char* field_name, const auto* member) {
      using MemberType =
          typename std::remove_cv_t<std::remove_reference_t<decltype(*member)>>;
      discoverTypesImpl<MemberType>(types, fields, field_name);
    };
    T dummy;
    SerializeMe::InvokeTypeDefinition(dummy, func);
    entry->second = std::move(fields);
  }
}

template <typename T, bool>
inline RegistrationID LogChannel::registerValue(const std::string& name,
                                                const T* value_ptr)
{
  std::lock_guard const lock(controlMutex());
  checkValueName(name);
  using namespace SerializeMe;
  static_assert(has_TypeDefinition<T>() || IsNumericType<T>(), "Missing TypeDefinition");

  if constexpr(IsNumericType<T>())
  {
    return registerValueImpl(name, ValuePtr(value_ptr), {});
  }
  else
  {
    PendingTypes types;
    discoverTypes<T>(types);
    auto def = typeRegistry().getSerializer<T>();
    return registerValueWithTypes(name, ValuePtr(value_ptr, def), def, std::move(types));
  }
}

template <typename T, std::enable_if_t<IsNumericType<T>(), bool>>
inline RegistrationID LogChannel::registerValue(const std::string& name,
                                                const std::atomic<T>* value_ptr)
{
  std::lock_guard const lock(controlMutex());
  checkValueName(name);
  return registerValueImpl(name, ValuePtr(value_ptr), {});
}

template <typename T>
inline RegistrationID LogChannel::registerCustomValue(const std::string& name,
                                                      const T* value_ptr,
                                                      CustomSerializer::Ptr serializer)
{
  std::lock_guard const lock(controlMutex());
  checkValueName(name);
  static_assert(!IsNumericType<T>(), "This method should be used only for custom types");
  if(!serializer)
  {
    throw std::invalid_argument("registerCustomValue: the serializer can not be null");
  }
  return registerValueImpl(name, ValuePtr(value_ptr, serializer), serializer);
}

template <template <class, class> class Container, class T, class... TArgs,
          std::enable_if_t<!has_TypeDefinition<Container<T, TArgs...>>::value, bool>>
inline RegistrationID LogChannel::registerValue(const std::string& prefix,
                                                const Container<T, TArgs...>* vect)
{
  std::lock_guard const lock(controlMutex());
  checkValueName(prefix);
  if constexpr(IsNumericType<T>())
  {
    return registerValueImpl(prefix, ValuePtr(vect), {});
  }
  else
  {
    PendingTypes types;
    discoverTypes<T>(types);
    auto def = typeRegistry().getSerializer<T>();
    return registerValueWithTypes(prefix, ValuePtr(vect), def, std::move(types));
  }
}

template <typename T, size_t N,
          std::enable_if_t<!has_TypeDefinition<std::array<T, N>>::value, bool>>
inline RegistrationID LogChannel::registerValue(const std::string& prefix,
                                                const std::array<T, N>* vect)
{
  std::lock_guard const lock(controlMutex());
  checkValueName(prefix);
  if constexpr(IsNumericType<T>())
  {
    return registerValueImpl(prefix, ValuePtr(vect), {});
  }
  else
  {
    PendingTypes types;
    discoverTypes<T>(types);
    auto def = typeRegistry().getSerializer<T>();
    // A fixed-size element is serialized like a vector's, with no virtual call per
    // element (SerializeMe sizes exactly). A variable-size one keeps its serializer,
    // which tells ValuePtr::isFixedSize(), and so startLogging(), that it can grow.
    auto value = def->isFixedSize() ? ValuePtr(vect) : ValuePtr(vect, def);
    return registerValueWithTypes(prefix, std::move(value), def, std::move(types));
  }
}

template <typename T>
inline std::shared_ptr<LoggedValue<T>>
LogChannel::createLoggedValue(std::string const& name, T initial_value)
{
  std::shared_ptr<LoggedValue<T>> logged(
      new LoggedValue<T>(shared_from_this(), initial_value));
  // Registered once the shared_ptr exists: if either step throws, the channel is
  // unchanged, and a value that was never registered has nothing to unregister.
  logged->id_ = registerValue(name, &logged->value_);
  return logged;
}

template <typename T>
inline LoggedValue<T>::LoggedValue(const std::shared_ptr<LogChannel>& channel,
                                   T initial_value)
  : state_(channel->sharedState()), channel_(channel), value_(initial_value)
{}

template <typename T>
inline LoggedValue<T>::~LoggedValue()
{
  if(id_ != RegistrationID{})
  {
    if(auto channel = channel_.lock())
    {
      channel->unregister(id_);
    }
  }
}

}  // namespace DataTamer
