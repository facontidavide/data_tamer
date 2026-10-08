#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeinfo>
#include <utility>

#include "data_tamer/types.hpp"
#include "data_tamer/contrib/SerializeMe.hpp"

namespace DataTamer
{

/**
 * @brief Serializes a type the library does not know (registerCustomValue()).
 * The snapshot path, tryTakeSnapshot() included, calls serializedSize(),
 * isFixedSize() and serialize(): they must not allocate, block or throw.
 *
 * ABI: the virtual functions are frozen for 2.x (the library calls them through
 * vtables compiled into user binaries) and the class keeps no data members. New
 * behaviour goes into a separate interface.
 */
class CustomSerializer
{
public:
  using Ptr = std::shared_ptr<CustomSerializer>;

  virtual ~CustomSerializer() = default;
  // Name of the type, as written in the schema: non-empty, without whitespace or
  // control characters, or the registration throws std::runtime_error.
  virtual const std::string& typeName() const = 0;

  // Optional opaque schema (encoding name and text) stored with the type.
  virtual std::optional<CustomSchema> typeSchema() const { return std::nullopt; }
  // Bytes serialize() writes for this instance; the snapshot buffer is sized with it.
  virtual size_t serializedSize(const void* instance) const = 0;

  // True if serializedSize() returns the same value for every instance.
  virtual bool isFixedSize() const = 0;

  // Writes exactly serializedSize(instance) bytes at the front of the buffer and
  // advances it.
  virtual void serialize(const void* instance, SerializeMe::SpanBytes&) const = 0;
};

//------------------------------------------------------------------

// Serializer that TypesRegistry builds for every type with a TypeDefinition.
template <typename T>
class CustomSerializerT : public CustomSerializer
{
public:
  explicit CustomSerializerT(std::string type_name);

  const std::string& typeName() const override;

  size_t serializedSize(const void* src_instance) const override;

  bool isFixedSize() const override;

  void serialize(const void* src_instance,
                 SerializeMe::SpanBytes& dst_buffer) const override;

private:
  std::string _name;
  size_t _fixed_size = 0;
};

/// Serializers of custom types by type name. Thread-safe.
class TypesRegistry
{
public:
  TypesRegistry();
  ~TypesRegistry();

  TypesRegistry(const TypesRegistry&) = delete;
  TypesRegistry& operator=(const TypesRegistry&) = delete;
  TypesRegistry(TypesRegistry&&) = delete;
  TypesRegistry& operator=(TypesRegistry&&) = delete;

  /// Stores a serializer of T under `type_name`, replacing any previous one. With
  /// skip_if_present an existing entry is kept and nullptr is returned; throws
  /// std::runtime_error if that entry belongs to another C++ type.
  template <typename T>
  CustomSerializer::Ptr addType(const std::string& type_name,
                                bool skip_if_present = false);

  /// The serializer stored under T's type name, created on first use. Throws
  /// std::runtime_error if the name belongs to another C++ type.
  template <typename T>
  [[nodiscard]] CustomSerializer::Ptr getSerializer();

private:
  using MakeSerializer = CustomSerializer::Ptr (*)(const std::string& type_name);

  template <typename T>
  static CustomSerializer::Ptr makeSerializer(const std::string& type_name);

  template <typename T>
  static void checkSameType(const CustomSerializer::Ptr& stored,
                            const std::string& type_name);

  // Both lock the registry and call make while holding the lock.
  CustomSerializer::Ptr findOrCreate(const std::string& type_name, MakeSerializer make);
  CustomSerializer::Ptr replace(const std::string& type_name, MakeSerializer make,
                                bool skip_if_present);

  struct Impl;
  std::unique_ptr<Impl> _impl;
};

//------------------------------------------------------------------
//------------------------------------------------------------------
//------------------------------------------------------------------

namespace details
{
/// GetBasicType<T>() for a recorded type. The one check, at compile time, that a numeric
/// type has a wire type (long double has none): every registration path reaches it.
template <typename T>
constexpr BasicType WireType()
{
  static_assert(!IsNumericType<T>() || GetBasicType<T>() != BasicType::OTHER, "numeric "
                                                                              "type has "
                                                                              "no wire "
                                                                              "type: use "
                                                                              "int, "
                                                                              "float or "
                                                                              "double");
  return GetBasicType<T>();
}
}  // namespace details

// Name of a custom type as written in the schema: TypeDefinitionTrait<T>::name() if
// provided, else the value returned by the definition of T. A std::string_view or
// const char* must outlive the program (a string literal). An owning string is cached
// here, but serialization calls the definition again and builds it every time.
template <typename T, typename = void>
struct CustomTypeName
{
  static std::string_view get()
  {
    // Only a numeric type without a wire type gets here, and WireType() refuses it.
    (void)details::WireType<T>();
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
        // An owning result would dangle as a view: cache it.
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

// A container is named after its elements, unless the type has a TypeDefinition itself.
template <template <class, class> class Container, class T, class... TArgs>
struct CustomTypeName<
    Container<T, TArgs...>,
    std::enable_if_t<!SerializeMe::has_TypeDefinition<Container<T, TArgs...>>::value>>
{
  static std::string_view get() { return CustomTypeName<T>::get(); }
};

template <typename T, size_t N>
struct CustomTypeName<
    std::array<T, N>,
    std::enable_if_t<!SerializeMe::has_TypeDefinition<std::array<T, N>>::value>>
{
  static std::string_view get() { return CustomTypeName<T>::get(); }
};

// Adds the serialized size of T to fixed_size, and clears is_fixed_size if T contains a
// vector. Used by the CustomSerializerT constructor.
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
      static_assert(has_TypeDefinition<T>(), "Missing TypeDefinition");
      auto funcA = [&](const char*, auto const* member) {
        using MemberType = std::remove_cv_t<std::remove_reference_t<decltype(*member)>>;
        GetFixedSize<MemberType>(is_fixed_size, fixed_size);
      };
      T dummy;
      InvokeTypeDefinition(dummy, funcA);
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
inline void TypesRegistry::checkSameType(const CustomSerializer::Ptr& stored,
                                         const std::string& type_name)
{
  if(typeid(*stored) != typeid(CustomSerializerT<T>))
  {
    throw std::runtime_error("custom type name '" + type_name +
                             "' is used by two C++ types: give each its own name");
  }
}

template <typename T>
inline CustomSerializer::Ptr TypesRegistry::getSerializer()
{
  static_assert(!IsNumericType<T>(), "You don't need to create a serializer for a "
                                     "numerical type.");

  const std::string type_name(CustomTypeName<T>::get());
  auto serializer = findOrCreate(type_name, &makeSerializer<T>);
  checkSameType<T>(serializer, type_name);
  return serializer;
}

template <typename T>
inline CustomSerializer::Ptr TypesRegistry::addType(const std::string& type_name,
                                                    bool skip_if_present)
{
  static_assert(!IsNumericType<T>(), "You don't need to create a serializer for a "
                                     "numerical type.");

  auto serializer = replace(type_name, &makeSerializer<T>, skip_if_present);
  if(!serializer)
  {
    // Skipped: the entry that holds the name must be this type's.
    checkSameType<T>(findOrCreate(type_name, &makeSerializer<T>), type_name);
  }
  return serializer;
}

}  // namespace DataTamer
