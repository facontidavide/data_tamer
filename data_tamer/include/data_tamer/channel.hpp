#pragma once

#include "data_tamer/values.hpp"
#include "data_tamer/data_sink.hpp"
#include "data_tamer/details/mutex.hpp"
#include "data_tamer/details/locked_reference.hpp"

#include <chrono>
#include <memory>

namespace DataTamer
{

// Utility
inline std::chrono::nanoseconds NsecSinceEpoch()
{
  auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch);
}

class DataSinkBase;
class LogChannel;
class ChannelsRegistry;

/**
 * @brief The LoggedValue class is a container of a variable that
 * automatically register/unregister to a Channel when created/destroyed.
 *
 * Setting and getting values is thread-safe.
 */
template <typename T>
class LoggedValue
{
protected:
  LoggedValue(const std::shared_ptr<LogChannel>& channel, const std::string& name,
              T initial_value);

  friend LogChannel;

public:
  LoggedValue() {}

  ~LoggedValue();

  LoggedValue(LoggedValue const& other) = delete;
  LoggedValue& operator=(LoggedValue const& other) = delete;

  LoggedValue(LoggedValue&& other) = default;
  LoggedValue& operator=(LoggedValue&& other) = default;

  /**
   * @brief set the value of the variable.
   *
   * @param value   new value
   * @param auto_enable  if true and the current instance is disabled, call setEnabled(true)
   */
  void set(const T& value, bool auto_enable = false);

  /// @brief get the stored value.
  [[nodiscard]] T get();

  [[nodiscard]] LockedRef<T, Mutex> getLockedReference();

  /// @brief Disabling a LoggedValue means that we will not record it in the snapshot
  void setEnabled(bool enabled);

  [[nodiscard]] bool isEnabled() const { return enabled_; }

private:
  std::weak_ptr<LogChannel> channel_;
  T value_ = {};
  RegistrationID id_;
  bool enabled_ = true;
};

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
   *
   * @param name   name of the value
   * @param value  pointer to the value
   * @return       the ID to be used to unregister or enable/disable this value.
   */
  template <typename T>
  RegistrationID registerValue(const std::string& name, const T* value);

  /**
   * @brief registerValue add a vectors of values.
   * You must guaranty that the pointer to each value is still valid,
   * when calling takeSnapshot.
   *
   * IMPORTANT / DANGER: do NOT resize the vector, once it is registered!
   *
   * @param name   name of the vector
   * @param value  pointer to the vectors of values.
   * @return       the ID to be used to unregister or enable/disable the values.
   */
  template <template <class, class> class Container, class T, class... TArgs>
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
  template <typename T, size_t N>
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
   */
  void addDataSink(std::shared_ptr<DataSinkBase> sink);

  /**
   * @brief takeSnapshot copies the current value of all your registered values
   *  and send an instance of Snapshot to all your Sinks.
   *
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
   * is already serialized.
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

private:
  struct Pimpl;
  std::unique_ptr<Pimpl> _p;

  TypesRegistry _type_registry;

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
  field.type = GetBasicType<T>();

  if constexpr (GetBasicType<T>() == BasicType::OTHER)
  {
    field.type_name = TypeDefinition<T>().typeName();

    if constexpr (container_info<T>::is_container)
    {
      field.is_vector = true;
      field.array_size = container_info<T>::size;
      using Type = typename container_info<T>::value_type;
      updateTypeRegistry<Type>();
    }
    else
    {
      updateTypeRegistry<T>();
    }
  }
  fields.push_back(field);
}

template <typename T>
inline void LogChannel::updateTypeRegistry()
{
  FieldsVector fields;

  if constexpr (!IsNumericType<T>())
  {
    auto funcPointer = [this, &fields](const char* field_name, const auto* member) {
      using MemberType =
          typename std::remove_cv_t<std::remove_reference_t<decltype(*member)>>;
      updateTypeRegistryImpl<MemberType>(fields, field_name);
    };

    auto funcMember = [this, &fields](const char* field_name, const auto& member) {
      using MemberType = decltype(getPointerType(member));
      updateTypeRegistryImpl<MemberType>(fields, field_name);
    };

    const auto& type_name = DataTamer::TypeDefinition<T>().typeName();
    auto added_serializer = _type_registry.addType<T>(type_name, true);
    if (added_serializer)
    {
      if constexpr (has_typedef_with_object<T>())
      {
        TypeDefinition<T>().typeDef({}, funcPointer);
      }
      else
      {
        TypeDefinition<T>().typeDef(funcMember);
      }
      // DataTamer::TypeDefinition<T>().typeDef({}, func);
      addCustomType(type_name, fields);
    }
  }
}

template <typename T>
inline RegistrationID LogChannel::registerValue(const std::string& name,
                                                const T* value_ptr)
{
  if constexpr (IsNumericType<T>())
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

template <typename T>
inline RegistrationID LogChannel::registerCustomValue(const std::string& name,
                                                      const T* value_ptr,
                                                      CustomSerializer::Ptr serializer)
{
  static_assert(!IsNumericType<T>(), "This method should be used only for custom types");

  return registerValueImpl(name, ValuePtr(value_ptr, serializer), serializer);
}

template <template <class, class> class Container, class T, class... TArgs>
inline RegistrationID LogChannel::registerValue(const std::string& prefix,
                                                const Container<T, TArgs...>* vect)
{
  if constexpr (IsNumericType<T>())
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

template <typename T, size_t N>
inline RegistrationID LogChannel::registerValue(const std::string& prefix,
                                                const std::array<T, N>* vect)
{
  if constexpr (IsNumericType<T>())
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
                                   const std::string& name, T initial_value) :
  channel_(channel), value_(initial_value), id_(channel->registerValue(name, &value_))
{}

template <typename T>
inline LoggedValue<T>::~LoggedValue()
{
  if (auto channel = channel_.lock())
  {
    channel->unregister(id_);
  }
}

template <typename T>
inline void LoggedValue<T>::setEnabled(bool enabled)
{
  if (auto channel = channel_.lock())
  {
    channel->setEnabled(id_, enabled);
  }
  enabled_ = enabled;
}

template <typename T>
inline void LoggedValue<T>::set(const T& val, bool auto_enable)
{
  if (auto channel = channel_.lock())
  {
    std::lock_guard const lock(channel->writeMutex());
    value_ = val;
    if (!enabled_ && auto_enable)
    {
      channel->setEnabled(id_, true);
      enabled_ = true;
    }
  }
  else
  {
    value_ = val;
    if (!enabled_ && auto_enable)
    {
      enabled_ = true;
    }
  }
}

template <typename T>
inline T LoggedValue<T>::get()
{
  if (auto channel = channel_.lock())
  {
    std::lock_guard const lock(channel->writeMutex());
    return value_;
  }
  return value_;
}

template <typename T>
inline LockedRef<T, Mutex> LoggedValue<T>::getLockedReference()
{
  Mutex* mutex_ptr = nullptr;
  if (auto chan = channel_.lock())
  {
    mutex_ptr = &chan->writeMutex();
  }
  return LockedRef<T, Mutex>(&value_, mutex_ptr);
}

}   // namespace DataTamer
