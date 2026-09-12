#pragma once

#include <atomic>
#include <cstring>
#include <typeindex>

#include "data_tamer/custom_types.hpp"
#include "data_tamer/contrib/SerializeMe.hpp"

namespace DataTamer
{
using SerializeMe::has_TypeDefinition;

namespace details
{
template <typename T>
struct is_std_atomic : std::false_type
{
};
template <typename T>
struct is_std_atomic<std::atomic<T>> : std::true_type
{
};
}  // namespace details

/**
 * @brief The ValuePtr is a non-owning pointer to a variable, together with
 * the two functions needed to serialize it. Type-erased through plain
 * function pointers (no std::function, no heap): the serializer for custom
 * types is kept alive by a shared_ptr member.
 */
class ValuePtr
{
public:
  using SerializeFn = void (*)(const void* value, const CustomSerializer* serializer,
                               SerializeMe::SpanBytes& dest);
  using SizeFn = size_t (*)(const void* value, const CustomSerializer* serializer);

  ValuePtr() = default;

  /// Plain value (numeric, or custom type with a serializer). std::atomic<T>
  /// is excluded here so it can only match the dedicated constructor below.
  template <typename T, std::enable_if_t<!details::is_std_atomic<T>::value, bool> = true>
  ValuePtr(const T* pointer, CustomSerializer::Ptr type_info = {});

  /// Atomic scalar: serialized with a relaxed load. Same BasicType and wire
  /// bytes as the plain T.
  template <typename T, std::enable_if_t<IsNumericType<T>(), bool> = true>
  ValuePtr(const std::atomic<T>* pointer);

  template <
      template <class, class> class Container, class T, class... TArgs,
      std::enable_if_t<!has_TypeDefinition<Container<T, TArgs...>>::value, bool> = true>
  ValuePtr(const Container<T, TArgs...>* vect);

  template <
      template <class, class> class Container, class T, class... TArgs,
      std::enable_if_t<!has_TypeDefinition<Container<T, TArgs...>>::value, bool> = true>
  ValuePtr(const Container<T, TArgs...>* vect, CustomSerializer::Ptr type_info);

  template <typename T, size_t N,
            std::enable_if_t<!has_TypeDefinition<std::array<T, N>>::value, bool> = true>
  ValuePtr(const std::array<T, N>* vect);

  template <typename T, size_t N,
            std::enable_if_t<!has_TypeDefinition<std::array<T, N>>::value, bool> = true>
  ValuePtr(const std::array<T, N>* vect, CustomSerializer::Ptr type_info);

  ValuePtr(ValuePtr const& other) = delete;
  ValuePtr& operator=(ValuePtr const& other) = delete;

  ValuePtr(ValuePtr&& other) noexcept = default;
  ValuePtr& operator=(ValuePtr&& other) noexcept = default;

  [[nodiscard]] bool operator==(const ValuePtr& other) const;
  [[nodiscard]] bool operator!=(const ValuePtr& other) const { return !(*this == other); }

  /// Release serialization ownership after quiescence, retaining slot type identity.
  void detach()
  {
    v_ptr_ = nullptr;
    serialize_fn_ = &ValuePtr::serializeNone;
    size_fn_ = &ValuePtr::sizeNone;
    serializer_.reset();
  }

  void serialize(SerializeMe::SpanBytes& dest) const;

  [[nodiscard]] size_t getSerializedSize() const;

  [[nodiscard]] BasicType type() const { return type_; }
  [[nodiscard]] bool isVector() const { return is_vector_; }
  [[nodiscard]] uint16_t vectorSize() const { return array_size_; }

private:
  const void* v_ptr_ = nullptr;
  SerializeFn serialize_fn_ = &ValuePtr::serializeNone;
  SizeFn size_fn_ = &ValuePtr::sizeNone;
  CustomSerializer::Ptr serializer_;  // keeps the custom serializer alive
  std::type_index type_index_ = typeid(void);
  BasicType type_ = BasicType::OTHER;
  bool is_vector_ = false;
  uint16_t array_size_ = 0;

  // ---- the type-erased implementations ----
  static void serializeNone(const void*, const CustomSerializer*, SerializeMe::SpanBytes&)
  {}
  static size_t sizeNone(const void*, const CustomSerializer*) { return 0; }

  template <typename T>
  static void serializeNumeric(const void* v, const CustomSerializer*,
                               SerializeMe::SpanBytes& dst)
  {
    std::memcpy(dst.data(), v, sizeof(T));
    dst.trimFront(sizeof(T));
  }
  template <typename T>
  static size_t sizeNumeric(const void*, const CustomSerializer*)
  {
    return sizeof(T);
  }

  template <typename T>
  static void serializeAtomic(const void* v, const CustomSerializer*,
                              SerializeMe::SpanBytes& dst)
  {
    const T tmp = static_cast<const std::atomic<T>*>(v)->load(std::memory_order_relaxed);
    std::memcpy(dst.data(), &tmp, sizeof(T));
    dst.trimFront(sizeof(T));
  }

  static void serializeCustom(const void* v, const CustomSerializer* s,
                              SerializeMe::SpanBytes& dst)
  {
    s->serialize(v, dst);
  }
  static size_t sizeCustom(const void* v, const CustomSerializer* s)
  {
    return s->serializedSize(v);
  }

  template <typename C>
  static void serializeContainer(const void* v, const CustomSerializer*,
                                 SerializeMe::SpanBytes& dst)
  {
    SerializeMe::SerializeIntoBuffer(dst, *static_cast<const C*>(v));
  }
  template <typename C>
  static size_t sizeContainer(const void* v, const CustomSerializer*)
  {
    return SerializeMe::BufferSize(*static_cast<const C*>(v));
  }

  template <typename C>
  static void serializeContainerCustom(const void* v, const CustomSerializer* s,
                                       SerializeMe::SpanBytes& dst)
  {
    const auto& vect = *static_cast<const C*>(v);
    SerializeMe::SerializeIntoBuffer(dst, uint32_t(vect.size()));
    for(const auto& value : vect)
    {
      s->serialize(&value, dst);
    }
  }
  template <typename C>
  static size_t sizeContainerCustom(const void* v, const CustomSerializer* s)
  {
    const auto& vect = *static_cast<const C*>(v);
    if(vect.empty())
    {
      return sizeof(uint32_t);
    }
    if(s->isFixedSize())
    {
      return sizeof(uint32_t) + vect.size() * s->serializedSize(&vect.front());
    }
    size_t tot = sizeof(uint32_t);
    for(const auto& value : vect)
    {
      tot += s->serializedSize(&value);
    }
    return tot;
  }

  template <typename A>
  static void serializeArrayCustom(const void* v, const CustomSerializer* s,
                                   SerializeMe::SpanBytes& dst)
  {
    for(const auto& value : *static_cast<const A*>(v))
    {
      s->serialize(&value, dst);
    }
  }
  template <typename A>
  static size_t sizeArrayCustom(const void* v, const CustomSerializer* s)
  {
    const auto& arr = *static_cast<const A*>(v);
    if(s->isFixedSize())
    {
      return arr.size() * s->serializedSize(&arr.front());
    }
    size_t tot = 0;
    for(const auto& value : arr)
    {
      tot += s->serializedSize(&value);
    }
    return tot;
  }
};

//------------------------------------------------------------
//------------------------------------------------------------
//------------------------------------------------------------

template <typename T, std::enable_if_t<!details::is_std_atomic<T>::value, bool>>
inline ValuePtr::ValuePtr(const T* pointer, CustomSerializer::Ptr type_info)
  : v_ptr_(pointer)
  , serializer_(std::move(type_info))
  , type_index_(typeid(T))
  , type_(GetBasicType<T>())
  , is_vector_(false)
{
  if(serializer_)
  {
    serialize_fn_ = &ValuePtr::serializeCustom;
    size_fn_ = &ValuePtr::sizeCustom;
  }
  else
  {
    serialize_fn_ = &ValuePtr::serializeNumeric<T>;
    size_fn_ = &ValuePtr::sizeNumeric<T>;
  }
}

template <typename T, std::enable_if_t<IsNumericType<T>(), bool>>
inline ValuePtr::ValuePtr(const std::atomic<T>* pointer)
  : v_ptr_(pointer)
  , serialize_fn_(&ValuePtr::serializeAtomic<T>)
  , size_fn_(&ValuePtr::sizeNumeric<T>)
  , type_index_(typeid(T))  // identical identity to the plain T, on purpose
  , type_(GetBasicType<T>())
  , is_vector_(false)
{
  static_assert(std::atomic<T>::is_always_lock_free, "atomic scalar must be lock-free");
}

template <template <class, class> class Container, class T, class... TArgs,
          std::enable_if_t<!has_TypeDefinition<Container<T, TArgs...>>::value, bool>>
inline ValuePtr::ValuePtr(const Container<T, TArgs...>* vect)
  : v_ptr_(vect)
  , serialize_fn_(&ValuePtr::serializeContainer<Container<T, TArgs...>>)
  , size_fn_(&ValuePtr::sizeContainer<Container<T, TArgs...>>)
  , type_index_(typeid(Container<T, TArgs...>))
  , type_(GetBasicType<T>())
  , is_vector_(true)
{}

template <template <class, class> class Container, class T, class... TArgs,
          std::enable_if_t<!has_TypeDefinition<Container<T, TArgs...>>::value, bool>>
inline ValuePtr::ValuePtr(const Container<T, TArgs...>* vect,
                          CustomSerializer::Ptr type_info)
  : v_ptr_(vect)
  , serialize_fn_(&ValuePtr::serializeContainerCustom<Container<T, TArgs...>>)
  , size_fn_(&ValuePtr::sizeContainerCustom<Container<T, TArgs...>>)
  , serializer_(std::move(type_info))
  , type_index_(typeid(Container<T, TArgs...>))
  , type_(GetBasicType<T>())
  , is_vector_(true)
{}

template <typename T, size_t N,
          std::enable_if_t<!has_TypeDefinition<std::array<T, N>>::value, bool>>
inline ValuePtr::ValuePtr(const std::array<T, N>* array)
  : v_ptr_(array)
  , serialize_fn_(&ValuePtr::serializeContainer<std::array<T, N>>)
  , size_fn_(&ValuePtr::sizeContainer<std::array<T, N>>)
  , type_index_(typeid(std::array<T, N>))
  , type_(GetBasicType<T>())
  , is_vector_(true)
  , array_size_(N)
{
  static_assert(N >= 1 && N <= 65535, "fixed array extent must be in 1..65535 (wire "
                                      "format)");
}

template <typename T, size_t N,
          std::enable_if_t<!has_TypeDefinition<std::array<T, N>>::value, bool>>
inline ValuePtr::ValuePtr(const std::array<T, N>* array, CustomSerializer::Ptr type_info)
  : v_ptr_(array)
  , serialize_fn_(&ValuePtr::serializeArrayCustom<std::array<T, N>>)
  , size_fn_(&ValuePtr::sizeArrayCustom<std::array<T, N>>)
  , serializer_(std::move(type_info))
  , type_index_(typeid(std::array<T, N>))
  , type_(GetBasicType<T>())
  , is_vector_(true)
  , array_size_(N)
{
  static_assert(N >= 1 && N <= 65535, "fixed array extent must be in 1..65535 (wire "
                                      "format)");
}

inline bool ValuePtr::operator==(const ValuePtr& other) const
{
  return type_ == other.type_ && type_index_ == other.type_index_ &&
         is_vector_ == other.is_vector_ && array_size_ == other.array_size_;
}

inline void ValuePtr::serialize(SerializeMe::SpanBytes& dest) const
{
  serialize_fn_(v_ptr_, serializer_.get(), dest);
}

inline size_t ValuePtr::getSerializedSize() const
{
  return size_fn_(v_ptr_, serializer_.get());
}

}  // namespace DataTamer
