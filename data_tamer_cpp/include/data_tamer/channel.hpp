#pragma once

#include "data_tamer/values.hpp"
#include "data_tamer/data_sink.hpp"
#include "data_tamer/logged_value.hpp"
#include "data_tamer/details/shared_state.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>

namespace DataTamer
{
using SerializeMe::has_TypeDefinition;

// Utility
inline std::chrono::nanoseconds NsecSinceEpoch()
{
  auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch);
}

class DataSinkBase;
class LogChannel;
class ChannelsRegistry;

//---------------------------------------------------------

/**
 * @brief A LogChannel is a class used to record multiple values in a single
 * "snapshot". Taking a snapshot is usually done in a periodic loop.
 *
 * Instances of LogChannel are accessed through the ChannelsRegistry.
 *
 * There is no limit to the number of tracked values, but sometimes you may
 * want to use different LogChannels in your application to:
 *
 * - aggregate them logically or
 * - take snapshots at different frequencies.
 *
 * Use the methods LogChannel::registerValue or LogChannel::createLoggedValue
 * to add a new value.
 * All you values must be registered before calling takeSnapshot for the first time.
 *
 */
class LogChannel : public std::enable_shared_from_this<LogChannel>
{
protected:
  // We make this private because the object must be wrapped
  // inside a std::shared_ptr.
  // This allows us to use std::weak_ptr in LoggedValue
  LogChannel(std::string name);

public:
  /// Use this static mentod do create an instance of LogChannel.
  /// it is recommended to use ChannelsRegistry::getChannel() instead
  static std::shared_ptr<LogChannel> create(std::string name);

  ~LogChannel();

  LogChannel(const LogChannel&) = delete;
  LogChannel& operator=(const LogChannel&) = delete;

  LogChannel(LogChannel&&) = delete;
  LogChannel& operator=(LogChannel&&) = delete;

  /**
   * @brief registerValue add a value to be monitored.
   * You must guaranty that the pointer to the value is still valid,
   * when calling takeSnapshot.
   * If you want to change the pointer T* to a new one,
   * you must first call unregister(), otherwise this method will throw
   * an exception.
   *
   * @param name   name of the value
   * @param value  pointer to the value
   * @return       the ID to be used to unregister or enable/disable this value.
   */
  template <typename T, bool = true>
  RegistrationID registerValue(const std::string& name, const T* value);

  /**
   * @brief registerValue for an atomic scalar. The value is read with a
   * relaxed load when the snapshot is taken; it serializes exactly like T.
   */
  template <typename T, std::enable_if_t<IsNumericType<T>(), bool> = true>
  RegistrationID registerValue(const std::string& name, const std::atomic<T>* value);

  /**
   * @brief registerValue add a vectors of values.
   * You must guaranty that the pointer to each value is still valid,
   * when calling takeSnapshot.
   *
   * @param name   name of the vector
   * @param value  pointer to the vectors of values.
   * @return       the ID to be used to unregister or enable/disable the values.
   */
  template <
      template <class, class> class Container, class T, class... TArgs,
      std::enable_if_t<!has_TypeDefinition<Container<T, TArgs...>>::value, bool> = true>
  RegistrationID registerValue(const std::string& name,
                               const Container<T, TArgs...>* value);

  /**
   * @brief registerValue add an array of values.
   * You must guaranty that the pointer to the array is still valid,
   * when calling takeSnapshot.
   *
   * @param name   name of the array
   * @param value  pointer to the array of values.
   * @return       the ID to be used to unregister or enable/disable the values.
   */
  template <typename T, size_t N,
            std::enable_if_t<!has_TypeDefinition<std::array<T, N>>::value, bool> = true>
  RegistrationID registerValue(const std::string& name, const std::array<T, N>* value);

  /**
   * @brief registerCustomValue should be used when you want to "bypass" the serialization
   * provided by DataTamer and use your own.
   *
   * This is an ADVANCED usage: using this approach does **not** guaranty that the application parsing the
   * data is able to deserialize it correctly. Sink mayl save the custom schema, but
   * it may or may not be enough.
   * Prefer the template specialization of RegisterVariable<T>, if you can.
   *
   * @param name      name of the array
   * @param value     pointer to the array of values.
   * @param type_info information needed to serialize this specific type.
   * @return          the ID to be used to unregister or enable/disable the values.
   */
  template <typename T>
  RegistrationID registerCustomValue(const std::string& name, const T* value,
                                     CustomSerializer::Ptr type_info);

  /**
   * @brief createLoggedValue is similar to registerValue(), but
   * the value is wrapped in a safer RAII interface. See LoggedValue for details.
   *
   * @param name of the value
   * @param initial_value  initial value to give to the LoggedValue
   *
   * @return the instance of LoggedValue, wrapped in a shared_ptr
   */
  template <typename T = double>
  [[nodiscard]] std::shared_ptr<LoggedValue<T>> createLoggedValue(std::string const& name,
                                                                  T initial_value = T{});

  /// Name of this channel (passed to the constructor)
  [[nodiscard]] const std::string& channelName() const;

  /** Enabling / disabling a value is much faster than
   *  registering / unregistering.
   *  It should be preferred when we want to temporary remove a
   *  value from the snapshot.
   */
  void setEnabled(const RegistrationID& id, bool enable);

  /// NOTE: the unregistered value will not be removed from the Schema
  void unregister(const RegistrationID& id);

  /**
   * @brief addDataSink add a sink, i.e. a class collecting our snapshots.
   * A channel holds at most eight sinks; adding a ninth throws. Adding the
   * same sink twice is a no-op.
   */
  void addDataSink(std::shared_ptr<DataSinkBase> sink);

  /**
   * @brief removeDataSink remove a sink, i.e. a class collecting our snapshots.
   */
  void removeDataSink(std::shared_ptr<DataSinkBase> sink);

  /**
  * @brief getNumberOfSinks returns the number of registered sinks.
  */
  size_t getNumberOfSinks() const;

  /**
   * @brief takeSnapshot copies the current value of all your registered values
   *  and send an instance of Snapshot to all your Sinks.
   *
   * Call from one snapshot producer thread per channel. Control operations
   * (registration, unregister, sink changes) must run outside writer guards
   * and serializer callbacks; they may wait for an active snapshot to finish.
   * @param timestamp is the time since epoch, by default.
   *
   * @return true is succesfully pushed to all its sinks.
   */
  bool takeSnapshot(std::chrono::nanoseconds timestamp = NsecSinceEpoch());

  /**
   * @brief getActiveFlags returns a serialized buffer, where
   * each bit represents if a series is enabled or not.
   * Therefore the vector size will be ceiling(series_count/8).
   *
   * Even if technically we may use vector<bool>, this data structure
   * is already serialized. This snapshot-thread-only view is the last rebuilt
   * mask, valid until the next snapshot or channel destruction.
   */
  [[nodiscard]] const ActiveMask& getActiveFlags();

  /**
   * @brief getSchema. See description of class Schema
   */
  [[nodiscard]] Schema getSchema() const;

  /** You will need to use this if:
   *
  * - your variables were registered using LogChannel::registerValue AND
  * - the variables are being modified in a thread different than the one calling takeSnapshot()
  *
  * No need to worry about LoggedValues (they use the mutex internally)
  */
  Mutex& writeMutex();

  /// Hold the channel write mutex so a group of writes appears in one
  /// snapshot or in none. Nested transactions on this thread are safe.
  [[nodiscard]] ChannelSharedState::Transaction scopedWrite();

  /// Snapshots that blocked after exhausting the write-mutex spin budget.
  [[nodiscard]] uint64_t writeLockContended() const;

  /// Longest blocking mutex acquisition after spin exhaustion, in nanoseconds.
  [[nodiscard]] uint64_t writeLockWaitMaxNs() const;

  /// Snapshot attempts that could not acquire a free pool slot.
  [[nodiscard]] uint64_t poolExhausted() const;

  /// Configure before the first snapshot (even one without sinks).
  /// Each slot reserves max(bytes, 2 * initial payload size, 256); zero is automatic.
  void setPayloadCapacity(size_t bytes);

  /// Number of retained/in-flight snapshots; default 64. Zero is invalid.
  void setPoolCapacity(size_t count);

  /// May change at runtime. Oversize snapshots return false instead of growing
  /// the acquired slot. Existing per-slot capacity is preserved. Serializer and
  /// allocator exceptions still propagate; strict mode is not a no-throw API.
  void setStrictMode(bool strict);

  /// Successful per-slot payload growth allocations after initial reservation.
  [[nodiscard]] uint64_t payloadReallocations() const;

  /// Snapshot attempts dropped because strict mode disallowed payload growth.
  [[nodiscard]] uint64_t droppedOversize() const;

  /// Failed publications to this attachment; zero if sink is not attached.
  [[nodiscard]] uint64_t
  droppedSnapshots(const std::shared_ptr<DataSinkBase>& sink) const;

  struct Stats
  {
    uint64_t write_lock_contended = 0;
    uint64_t write_lock_wait_max_ns = 0;
    uint64_t pool_exhausted = 0;
    uint64_t payload_reallocations = 0;
    uint64_t dropped_oversize = 0;
  };

  [[nodiscard]] Stats stats() const;

  /// State shared with this channel's LoggedValues (enable flags, write mutex).
  [[nodiscard]] std::shared_ptr<ChannelSharedState> sharedState() const;

private:
  struct Pimpl;
  std::unique_ptr<Pimpl> _p;

  TypesRegistry _type_registry;

  std::mutex& controlMutex();
  bool schemaFrozen() const;
  bool hasCustomType(const std::string& type_name) const;

  template <typename T>
  void updateTypeRegistry();

  template <typename T>
  void updateTypeRegistryImpl(FieldsVector& fields, const char* name);

  void addCustomType(const std::string& custom_type_name, const FieldsVector& fields);

  [[nodiscard]] RegistrationID registerValueImpl(const std::string& name,
                                                 ValuePtr&& value_ptr,
                                                 CustomSerializer::Ptr type_info);
};

//----------------------------------------------------------------------
//----------------------------------------------------------------------
//----------------------------------------------------------------------

template <typename T>
void LogChannel::updateTypeRegistryImpl(FieldsVector& fields, const char* field_name)
{
  using SerializeMe::container_info;
  TypeField field;
  field.field_name = field_name;

  if constexpr(container_info<T>::is_container)
  {
    // A container member is described by its element type, like a top-level
    // registerValue() of the same container: "float64[3] axis", "Pose[] poses".
    using Type = typename container_info<T>::value_type;
    field.is_vector = true;
    field.array_size = container_info<T>::size;
    field.type = GetBasicType<Type>();
    if constexpr(GetBasicType<Type>() == BasicType::OTHER)
    {
      field.type_name = CustomTypeName<Type>::get();
      updateTypeRegistry<Type>();
    }
    else
    {
      field.type_name = ToStr(field.type);
    }
  }
  else
  {
    field.type = GetBasicType<T>();
    if constexpr(GetBasicType<T>() == BasicType::OTHER)
    {
      field.type_name = CustomTypeName<T>::get();
      updateTypeRegistry<T>();
    }
    else
    {
      field.type_name = ToStr(field.type);
    }
  }
  fields.push_back(field);
}

template <typename T>
inline void LogChannel::updateTypeRegistry()
{
  if constexpr(!IsNumericType<
                   T>())  // everything below must not be instantiated for numbers
  {
    using namespace SerializeMe;
    static_assert(has_TypeDefinition<T>(), "Missing TypeDefinition");

    FieldsVector fields;
    const std::string type_name(CustomTypeName<T>::get());
    if(schemaFrozen())
    {
      if(!hasCustomType(type_name))
        throw std::runtime_error("Can't add a custom type after recording started");
      return;
    }
    if(auto added_serializer = _type_registry.addType<T>(type_name, true))
    {
      auto func = [this, &fields](const char* field_name, const auto* member) {
        using MemberType =
            typename std::remove_cv_t<std::remove_reference_t<decltype(*member)>>;
        updateTypeRegistryImpl<MemberType>(fields, field_name);
      };
      T dummy;
      TypeDefinition(dummy, func);
      addCustomType(type_name, fields);
    }
  }
}

template <typename T, bool>
inline RegistrationID LogChannel::registerValue(const std::string& name,
                                                const T* value_ptr)
{
  std::lock_guard const lock(controlMutex());
  using namespace SerializeMe;
  static_assert(has_TypeDefinition<T>() || IsNumericType<T>(), "Missing TypeDefinition");

  if constexpr(IsNumericType<T>())
  {
    return registerValueImpl(name, ValuePtr(value_ptr), {});
  }
  else
  {
    updateTypeRegistry<T>();
    auto def = _type_registry.getSerializer<T>();
    return registerValueImpl(name, ValuePtr(value_ptr, def), def);
  }
}

template <typename T, std::enable_if_t<IsNumericType<T>(), bool>>
inline RegistrationID LogChannel::registerValue(const std::string& name,
                                                const std::atomic<T>* value_ptr)
{
  std::lock_guard const lock(controlMutex());
  return registerValueImpl(name, ValuePtr(value_ptr), {});
}

template <typename T>
inline RegistrationID LogChannel::registerCustomValue(const std::string& name,
                                                      const T* value_ptr,
                                                      CustomSerializer::Ptr serializer)
{
  std::lock_guard const lock(controlMutex());
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
  if constexpr(IsNumericType<T>())
  {
    return registerValueImpl(prefix, ValuePtr(vect), {});
  }
  else
  {
    updateTypeRegistry<T>();
    auto def = _type_registry.getSerializer<T>();
    return registerValueImpl(prefix, ValuePtr(vect), def);
  }
}

template <typename T, size_t N,
          std::enable_if_t<!has_TypeDefinition<std::array<T, N>>::value, bool>>
inline RegistrationID LogChannel::registerValue(const std::string& prefix,
                                                const std::array<T, N>* vect)
{
  std::lock_guard const lock(controlMutex());
  if constexpr(IsNumericType<T>())
  {
    return registerValueImpl(prefix, ValuePtr(vect), {});
  }
  else
  {
    updateTypeRegistry<T>();
    auto def = _type_registry.getSerializer<T>();
    return registerValueImpl(prefix, ValuePtr(vect, def), def);
  }
}

template <typename T>
inline std::shared_ptr<LoggedValue<T>>
LogChannel::createLoggedValue(std::string const& name, T initial_value)
{
  auto val = new LoggedValue<T>(shared_from_this(), name, initial_value);
  return std::shared_ptr<LoggedValue<T>>(val);
}

template <typename T>
inline LoggedValue<T>::LoggedValue(const std::shared_ptr<LogChannel>& channel,
                                   const std::string& name, T initial_value)
  : state_(channel->sharedState())
  , channel_(channel)
  , value_(initial_value)
  , id_(channel->registerValue(name, &value_))
{}

template <typename T>
inline LoggedValue<T>::~LoggedValue()
{
  if(auto channel = channel_.lock())
  {
    channel->unregister(id_);
  }
}

template <typename T>
inline void LoggedValue<T>::setEnabled(bool enabled)
{
  state_->setEnabled(id_, enabled);
}

template <typename T>
inline bool LoggedValue<T>::isEnabled() const
{
  return state_->isEnabled(id_.first_index);
}

template <typename T>
inline void LoggedValue<T>::set(const T& val, bool auto_enable)
{
  if constexpr(kAtomic)
  {
    value_.store(val, std::memory_order_relaxed);
  }
  else
  {
    ChannelSharedState::Transaction transaction(*state_);
    value_ = val;
  }
  if(auto_enable && !isEnabled())
  {
    setEnabled(true);
  }
}

template <typename T>
inline T LoggedValue<T>::get() const
{
  if constexpr(kAtomic)
  {
    return value_.load(std::memory_order_relaxed);
  }
  else
  {
    ChannelSharedState::Transaction transaction(*state_);
    return value_;
  }
}

template <typename T>
template <typename U, std::enable_if_t<is_atomic_scalar_v<U>, bool>>
inline AtomicProxy<T> LoggedValue<T>::getMutablePtr()
{
  return AtomicProxy<T>(&value_);
}

template <typename T>
template <typename U, std::enable_if_t<!is_atomic_scalar_v<U>, bool>>
inline MutablePtr<T> LoggedValue<T>::getMutablePtr()
{
  return MutablePtr<T>(&value_, &state_->write_mutex);
}

template <typename T>
template <typename U, std::enable_if_t<is_atomic_scalar_v<U>, bool>>
inline AtomicConstProxy<T> LoggedValue<T>::getConstPtr()
{
  return AtomicConstProxy<T>(&value_);
}

template <typename T>
template <typename U, std::enable_if_t<!is_atomic_scalar_v<U>, bool>>
inline ConstPtr<T> LoggedValue<T>::getConstPtr()
{
  return ConstPtr<T>(&value_, &state_->write_mutex);
}

}  // namespace DataTamer
