#pragma once

#include <memory>
#include <optional>

#include "data_tamer/types.hpp"
#include "data_tamer/contrib/SerializeMe.hpp"

namespace DataTamer
{

/**
 * @brief Serializes a type the library does not know (registerCustomValue()).
 *
 * ABI: the virtual functions below are frozen for 2.x. The library calls them
 * through vtables compiled into user binaries, so adding, removing or
 * reordering one breaks every serializer built against an earlier 2.x release.
 * New behaviour arrives as a separate interface; CustomSerializer keeps no
 * data members.
 */
class CustomSerializer
{
public:
  using Ptr = std::shared_ptr<CustomSerializer>;

  virtual ~CustomSerializer() = default;
  // name of the type, to be written in the schema string.
  virtual const std::string& typeName() const = 0;

  // optional custom schema of the type
  virtual std::optional<CustomSchema> typeSchema() const { return std::nullopt; }
  // size in bytes of the serialized object.
  // Needed to pre-allocate memory in the buffer
  virtual size_t serializedSize(const void* instance) const = 0;

  // true if the method serializedSize will ALWAYS return the same value
  virtual bool isFixedSize() const = 0;

  // serialize an object into a buffer.
  virtual void serialize(const void* instance, SerializeMe::SpanBytes&) const = 0;
};

//------------------------------------------------------------------

// This derived class is used automatically by all the types
// that have a template specialization of TypeDefinition<T>
template <typename T>
class CustomSerializerT : public CustomSerializer
{
public:
  CustomSerializerT(std::string type_name);

  const std::string& typeName() const override;

  size_t serializedSize(const void* src_instance) const override;

  bool isFixedSize() const override;

  void serialize(const void* src_instance,
                 SerializeMe::SpanBytes& dst_buffer) const override;

private:
  std::string _name;
  size_t _fixed_size = 0;
};

class TypesRegistry
{
public:
  // The state lives behind a Pimpl, which does not allow default special members.
  TypesRegistry();
  ~TypesRegistry();

  TypesRegistry(const TypesRegistry&) = delete;
  TypesRegistry& operator=(const TypesRegistry&) = delete;
  TypesRegistry(TypesRegistry&&) = delete;
  TypesRegistry& operator=(TypesRegistry&&) = delete;

  template <typename T>
  CustomSerializer::Ptr addType(const std::string& type_name,
                                bool skip_if_present = false);

  template <typename T>
  [[nodiscard]] CustomSerializer::Ptr getSerializer();

private:
  // Builds the CustomSerializerT<T> of a type; the registry stores the result.
  using MakeSerializer = CustomSerializer::Ptr (*)(const std::string& type_name);

  template <typename T>
  static CustomSerializer::Ptr makeSerializer(const std::string& type_name);

  // Both lock the registry and call make while holding the lock.
  // Returns the stored serializer, creating it first when missing.
  CustomSerializer::Ptr findOrCreate(const std::string& type_name, MakeSerializer make);
  // Stores a new serializer, replacing a previous one. Returns {} without touching
  // the registry when skip_if_present is set and the type is already there.
  CustomSerializer::Ptr replace(const std::string& type_name, MakeSerializer make,
                                bool skip_if_present);

  struct Impl;
  std::unique_ptr<Impl> _impl;
};

//------------------------------------------------------------------
//------------------------------------------------------------------
//------------------------------------------------------------------

// Name of a custom type, as written in the schema.
// It comes from DataTamer::TypeDefinitionTrait<T>::name() when provided,
// otherwise from the value returned by the definition of T.
// A definition returning std::string_view or const char* must point to
// storage that outlives the program (e.g. a string literal). If it returns
// an owning string (e.g. std::string), it is evaluated once and the result is
// cached, like name().
template <typename T>
struct CustomTypeName
{
  static std::string_view get()
  {
    static_assert(SerializeMe::has_TypeDefinition<T>(), "Missing TypeDefinition");
    if constexpr(SerializeMe::has_TypeDefinitionTrait<T>::value &&
                 SerializeMe::has_TypeDefinitionTraitName<T>::value)
    {
      static const std::string name = TypeDefinitionTrait<T>::name();
      return name;
    }
    else
    {
      static_assert(std::is_default_constructible_v<T>, "Must be default constructible");
      using Result = std::decay_t<decltype(SerializeMe::InvokeTypeDefinition(
          std::declval<T&>(), std::declval<SerializeMe::EmptyFunc&>()))>;
      if constexpr(std::is_same_v<Result, std::string_view> ||
                   std::is_same_v<Result, const char*>)
      {
        T dummy;
        return SerializeMe::InvokeTypeDefinition(dummy, SerializeMe::EmptyFuncion);
      }
      else
      {
        // An owning result would dangle once returned as a view: keep it.
        static_assert(std::is_constructible_v<std::string, Result>, "TypeDefinition must "
                                                                    "return the type "
                                                                    "name");
        static const std::string name = []() {
          T dummy;
          return std::string(
              SerializeMe::InvokeTypeDefinition(dummy, SerializeMe::EmptyFuncion));
        }();
        return name;
      }
    }
  }
};

template <template <class, class> class Container, class T, class... TArgs>
struct CustomTypeName<Container<T, TArgs...>>
{
  static std::string_view get() { return CustomTypeName<T>::get(); }
};

template <typename T, size_t N>
struct CustomTypeName<std::array<T, N>>
{
  static std::string_view get() { return CustomTypeName<T>::get(); }
};

template <class C, typename T>
T getPointerType(T C::*v);

// Recursive function to compute if a type has fixed size (at compile time).
// Used mainly by the CustomSerializerT constructor.
template <typename T>
inline void GetFixedSize(bool& is_fixed_size, size_t& fixed_size)
{
  using namespace SerializeMe;

  if constexpr(IsNumericType<T>())
  {
    fixed_size += sizeof(T);
  }
  else
  {
    constexpr auto info = container_info<T>();
    if constexpr(info.is_container && info.size == 0)
    {
      // vector
      is_fixed_size = false;
    }
    else if constexpr(info.is_container && info.size >= 0)
    {
      // array
      size_t obj_size = 0;
      using Type = typename container_info<T>::value_type;
      GetFixedSize<Type>(is_fixed_size, obj_size);
      fixed_size += info.size * obj_size;
    }
    else if(is_fixed_size)
    {
      if constexpr(has_TypeDefinition<T>())
      {
        auto funcA = [&](const char*, auto const* member) {
          using MemberType = std::remove_cv_t<std::remove_reference_t<decltype(*member)>>;
          GetFixedSize<MemberType>(is_fixed_size, fixed_size);
        };
        T dummy;
        InvokeTypeDefinition(dummy, funcA);
      }
      else
      {
        throw std::logic_error("Missing TypeDefinition");
      }
    }
  }
}

template <typename T>
inline CustomSerializerT<T>::CustomSerializerT(std::string type_name)
  : _name(std::move(type_name))
{
  bool is_fixed_size = true;
  GetFixedSize<T>(is_fixed_size, _fixed_size);
  if(!is_fixed_size)
  {
    _fixed_size = 0;
  }
}

template <typename T>
inline const std::string& CustomSerializerT<T>::typeName() const
{
  return _name;
}

template <typename T>
inline size_t CustomSerializerT<T>::serializedSize(const void* src_instance) const
{
  if(_fixed_size != 0)
  {
    return _fixed_size;
  }
  const auto* obj = static_cast<const T*>(src_instance);
  return SerializeMe::BufferSize(*obj);
}

template <typename T>
inline bool CustomSerializerT<T>::isFixedSize() const
{
  return _fixed_size > 0;
}

template <typename T>
inline void CustomSerializerT<T>::serialize(const void* src_instance,
                                            SerializeMe::SpanBytes& dst_buffer) const
{
  const auto* obj = static_cast<const T*>(src_instance);
  SerializeMe::SerializeIntoBuffer(dst_buffer, *obj);
}

template <typename T>
inline CustomSerializer::Ptr TypesRegistry::makeSerializer(const std::string& type_name)
{
  return std::make_shared<CustomSerializerT<T>>(type_name);
}

template <typename T>
inline CustomSerializer::Ptr TypesRegistry::getSerializer()
{
  static_assert(!IsNumericType<T>(), "You don't need to create a serializer for a "
                                     "numerical type.");

  const std::string type_name(CustomTypeName<T>::get());
  return findOrCreate(type_name, &makeSerializer<T>);
}

template <typename T>
inline CustomSerializer::Ptr TypesRegistry::addType(const std::string& type_name,
                                                    bool skip_if_present)
{
  static_assert(!IsNumericType<T>(), "You don't need to create a serializer for a "
                                     "numerical type.");

  return replace(type_name, &makeSerializer<T>, skip_if_present);
}

}  // namespace DataTamer
