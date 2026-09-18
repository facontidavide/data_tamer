#pragma once

#include <functional>
#include <cstring>
#include <typeindex>
#include <type_traits>

#include "data_tamer/custom_types.hpp"
#include "data_tamer/contrib/SerializeMe.hpp"

#if DATA_TAMER_EIGEN_SUPPORT
#include <Eigen/Core>
#endif

namespace DataTamer
{
using SerializeMe::has_TypeDefinition;

#if DATA_TAMER_EIGEN_SUPPORT
/// Any Eigen type: plain objects, views (Block, Map, Ref) and expressions.
template <class T>
inline constexpr bool IsEigenType =
    std::is_base_of_v<Eigen::EigenBase<std::remove_cv_t<T>>, std::remove_cv_t<T>>;

/// Eigen::Matrix and Eigen::Array, i.e. the Eigen types that own their storage.
/// Only these can be registered, because the channel keeps the pointer and
/// dereferences it at every snapshot.
template <class T>
inline constexpr bool IsEigenPlainObject =
    std::is_base_of_v<Eigen::PlainObjectBase<std::remove_cv_t<T>>, std::remove_cv_t<T>>;
#else
template <class T>
inline constexpr bool IsEigenType = false;
#endif

/**
 * @brief The ValuePtr is a non-owning pointer to
 * a variable.
 */
class ValuePtr
{
public:
  ValuePtr() = default;

  template <typename T, std::enable_if_t<!IsEigenType<T>, bool> = true>
  ValuePtr(const T* pointer, CustomSerializer::Ptr type_info = {});

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

#if DATA_TAMER_EIGEN_SUPPORT
  template <class Derived, std::enable_if_t<IsEigenPlainObject<Derived>, bool> = true>
  ValuePtr(const Derived* vect);
#endif

  ValuePtr(ValuePtr const& other) = delete;
  ValuePtr& operator=(ValuePtr const& other) = delete;

  ValuePtr(ValuePtr&& other) = default;
  ValuePtr& operator=(ValuePtr&& other) = default;

  [[nodiscard]] bool operator==(const ValuePtr& other) const;

  [[nodiscard]] bool operator!=(const ValuePtr& other) const { return !(*this == other); }

  void serialize(SerializeMe::SpanBytes& dest) const;

  [[nodiscard]] size_t getSerializedSize() const;

  /// Get the type of the stored variable pointer
  [[nodiscard]] BasicType type() const { return type_; }

  [[nodiscard]] bool isVector() const { return is_vector_; }

  [[nodiscard]] uint16_t vectorSize() const { return array_size_; }

private:
  const void* v_ptr_ = nullptr;
  BasicType type_ = BasicType::OTHER;
  std::type_index type_index_ = typeid(void);
  std::uint8_t memory_size_ = 0;
  std::function<void(SerializeMe::SpanBytes&)> serialize_impl_;
  std::function<size_t()> get_size_impl_;
  bool is_vector_ = false;
  uint16_t array_size_ = 0;
};

//------------------------------------------------------------
//------------------------------------------------------------
//------------------------------------------------------------

template <typename T, std::enable_if_t<!IsEigenType<T>, bool>>
inline ValuePtr::ValuePtr(const T* pointer, CustomSerializer::Ptr type_info)
  : v_ptr_(pointer)
  , type_(GetBasicType<T>())
  , type_index_(typeid(T))
  , memory_size_(sizeof(T))
  , is_vector_(false)
{
  if(type_info)
  {
    serialize_impl_ = [type_info, pointer](SerializeMe::SpanBytes& buffer) -> void {
      type_info->serialize(pointer, buffer);
    };
    get_size_impl_ = [type_info, pointer]() -> size_t {
      return type_info->serializedSize(pointer);
    };
  }
}

template <template <class, class> class Container, class T, class... TArgs,
          std::enable_if_t<!has_TypeDefinition<Container<T, TArgs...>>::value, bool>>
inline ValuePtr::ValuePtr(const Container<T, TArgs...>* vect)
  : v_ptr_(vect)
  , type_(GetBasicType<T>())
  , type_index_(typeid(Container<T, TArgs...>))
  , memory_size_(sizeof(T))
  , is_vector_(true)
{
  serialize_impl_ = [vect](SerializeMe::SpanBytes& buffer) -> void {
    SerializeMe::SerializeIntoBuffer(buffer, *vect);
  };
  get_size_impl_ = [vect]() -> size_t { return SerializeMe::BufferSize(*vect); };
}

template <template <class, class> class Container, class T, class... TArgs,
          std::enable_if_t<!has_TypeDefinition<Container<T, TArgs...>>::value, bool>>
inline ValuePtr::ValuePtr(const Container<T, TArgs...>* vect,
                          CustomSerializer::Ptr type_info)
  : v_ptr_(vect)
  , type_(GetBasicType<T>())
  , type_index_(typeid(Container<T, TArgs...>))
  , memory_size_(sizeof(T))
  , is_vector_(true)
{
  serialize_impl_ = [type_info, vect](SerializeMe::SpanBytes& buffer) -> void {
    SerializeMe::SerializeIntoBuffer(buffer, uint32_t(vect->size()));
    for(const auto& value : (*vect))
    {
      type_info->serialize(&value, buffer);
    }
  };
  get_size_impl_ = [type_info, vect]() -> size_t {
    if(vect->empty())
    {
      return sizeof(uint32_t);
    }
    if(type_info->isFixedSize())
    {
      return sizeof(uint32_t) + vect->size() * type_info->serializedSize(&vect->front());
    }
    size_t tot_size = sizeof(uint32_t);
    for(const auto& value : (*vect))
    {
      tot_size += type_info->serializedSize(&value);
    }
    return tot_size;
  };
}

template <typename T, size_t N,
          std::enable_if_t<!has_TypeDefinition<std::array<T, N>>::value, bool>>
inline ValuePtr::ValuePtr(const std::array<T, N>* array)
  : v_ptr_(array)
  , type_(GetBasicType<T>())
  , type_index_(typeid(std::array<T, N>))
  , is_vector_(true)
  , array_size_(N)
{
  serialize_impl_ = [array](SerializeMe::SpanBytes& buffer) -> void {
    SerializeMe::SerializeIntoBuffer(buffer, *array);
  };
  get_size_impl_ = [array]() { return SerializeMe::BufferSize(*array); };
}

template <typename T, size_t N,
          std::enable_if_t<!has_TypeDefinition<std::array<T, N>>::value, bool>>
inline ValuePtr::ValuePtr(const std::array<T, N>* array, CustomSerializer::Ptr type_info)
  : v_ptr_(array)
  , type_(GetBasicType<T>())
  , type_index_(typeid(std::array<T, N>))
  , is_vector_(true)
  , array_size_(N)
{
  serialize_impl_ = [type_info, array](SerializeMe::SpanBytes& buffer) -> void {
    for(const auto& value : (*array))
    {
      type_info->serialize(&value, buffer);
    }
  };
  get_size_impl_ = [type_info, array]() {
    size_t tot_size = 0;
    if(type_info->isFixedSize())
    {
      return N * type_info->serializedSize(&array->front());
    }
    for(const auto& value : (*array))
    {
      tot_size += type_info->serializedSize(&value);
    }
    return tot_size;
  };
}

#if DATA_TAMER_EIGEN_SUPPORT
template <class Derived, std::enable_if_t<IsEigenPlainObject<Derived>, bool>>
inline ValuePtr::ValuePtr(const Derived* vect)
  : v_ptr_(vect)
  , type_(GetBasicType<typename Derived::Scalar>())
  , type_index_(typeid(Derived))
  , memory_size_(sizeof(typename Derived::Scalar))
  , is_vector_(true)
  , array_size_(Derived::SizeAtCompileTime == Eigen::Dynamic ?
                    0 :
                    static_cast<uint16_t>(Derived::SizeAtCompileTime))
{
  using Scalar = typename Derived::Scalar;
  constexpr bool kFixedSize = Derived::SizeAtCompileTime != Eigen::Dynamic;

  // clang-format off
  static_assert(GetBasicType<Scalar>() != BasicType::OTHER,
                "DataTamer: unsupported Eigen scalar type");
  static_assert(Derived::IsVectorAtCompileTime,
                "DataTamer: only Eigen vectors are supported, not matrices. Flattening "
                "a matrix would lose its shape and record the coefficients in Eigen's "
                "storage order.");
  static_assert(!kFixedSize || Derived::SizeAtCompileTime <= 65535,
                "DataTamer: fixed-size vectors are limited to 65535 elements");
  // clang-format on

  serialize_impl_ = [vect](SerializeMe::SpanBytes& buffer) -> void {
    const auto count = static_cast<uint32_t>(vect->size());
    if constexpr(!kFixedSize)
    {
      SerializeMe::SerializeIntoBuffer(buffer, count);
    }
    for(uint32_t i = 0; i < count; i++)
    {
      SerializeMe::SerializeIntoBuffer(buffer, (*vect)[i]);
    }
  };

  get_size_impl_ = [vect]() -> size_t {
    const size_t prefix = kFixedSize ? 0 : sizeof(uint32_t);
    return prefix + static_cast<size_t>(vect->size()) * sizeof(Scalar);
  };
}
#endif

inline bool ValuePtr::operator==(const ValuePtr& other) const
{
  return type_ == other.type_ && type_index_ == other.type_index_ &&
         is_vector_ == other.is_vector_ && array_size_ == other.array_size_;
}

inline void ValuePtr::serialize(SerializeMe::SpanBytes& dest) const
{
  if(serialize_impl_)
  {
    serialize_impl_(dest);
    return;
  }
  std::memcpy(dest.data(), v_ptr_, memory_size_);
  dest.trimFront(memory_size_);
}

inline size_t ValuePtr::getSerializedSize() const
{
  return (get_size_impl_) ? get_size_impl_() : memory_size_;
}

}  // namespace DataTamer
