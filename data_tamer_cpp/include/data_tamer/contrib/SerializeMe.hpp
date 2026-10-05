/**
 * MIT License
 *
 * Copyright (c) 2019-2024 Davide Faconti
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 *all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 **/

#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace SerializeMe
{

// Poor man version of Span
template <typename T>
class Span
{
public:
  Span() = default;

  Span(T* ptr, size_t size) : data_(ptr), size_(size) {}

  template <size_t N>
  Span(std::array<T, N>& v) : data_(v.data()), size_(N)
  {}

  Span(std::vector<T>& v) : data_(v.data()), size_(v.size()) {}

  T const* data() const;

  T* data();

  size_t size() const;

  void trimFront(size_t offset);

private:
  T* data_ = nullptr;
  size_t size_ = 0;
};

using SpanBytes = Span<uint8_t>;
using SpanBytesConst = Span<uint8_t const>;
using StringSize = uint16_t;

const auto EmptyFuncion = [](const char*, void*) {};
using EmptyFunc = decltype(EmptyFuncion);

template <typename T1, typename T2>
using enable_if_same_t = std::enable_if_t<std::is_same_v<T1, T2>>;

}  // namespace SerializeMe

namespace DataTamer
{
/**
 * @brief Customization point that describes a type without reopening its namespace.
 *
 * Specialize it (fully, or partially using the second parameter for
 * std::enable_if / std::void_t) in namespace DataTamer:
 *
 *   template <>
 *   struct DataTamer::TypeDefinitionTrait<third_party::Point>
 *   {
 *     template <class AddField>
 *     static std::string_view define(third_party::Point& p, AddField& add)
 *     {
 *       add("x", &p.x);
 *       add("y", &p.y);
 *       return "Point";
 *     }
 *   };
 *
 * Optionally, the specialization may also provide
 *
 *   static std::string name();
 *
 * to build the type name at runtime (useful for class templates, e.g.
 * "Vector" + std::to_string(N)). It is evaluated once per type (per shared
 * library, as for any function-local static) and cached;
 * when present, the value returned by define() is ignored and define() may
 * return void.
 *
 * define() may also return an owning string (e.g. std::string): it is then
 * evaluated once per type and cached.
 *
 * If a type has both a TypeDefinitionTrait specialization and a
 * TypeDefinition() overload found by argument-dependent lookup, the trait wins.
 * A specialization whose define() can not be called as
 * define(T&, AddField&) is a compile error (it is never silently ignored).
 *
 * The specialization must be visible before the type is first used with
 * DataTamer (as for any template specialization).
 */
template <typename T, typename = void>
struct TypeDefinitionTrait
{
  // Marks the primary template: a specialization does not have it.
  using unspecialized_tag = void;
};
}  // namespace DataTamer

namespace SerializeMe
{

// True if DataTamer::TypeDefinitionTrait<T> is specialized with a
// static define(T&, AddField&) function.
template <typename T, class = void>
struct has_TypeDefinitionTrait : std::false_type
{
};

template <typename T>
struct has_TypeDefinitionTrait<
    T, std::void_t<decltype(DataTamer::TypeDefinitionTrait<T>::define(
           std::declval<T&>(), std::declval<EmptyFunc&>()))>> : std::true_type
{
};

// True if DataTamer::TypeDefinitionTrait<T> is specialized at all.
template <typename T, class = void>
struct is_TypeDefinitionTrait_specialized : std::true_type
{
};

template <typename T>
struct is_TypeDefinitionTrait_specialized<
    T, std::void_t<typename DataTamer::TypeDefinitionTrait<T>::unspecialized_tag>>
  : std::false_type
{
};

// True if DataTamer::TypeDefinitionTrait<T> provides static name().
template <typename T, class = void>
struct has_TypeDefinitionTraitName : std::false_type
{
};

template <typename T>
struct has_TypeDefinitionTraitName<
    T, std::enable_if_t<std::is_convertible_v<
           decltype(DataTamer::TypeDefinitionTrait<T>::name()), std::string>>>
  : std::true_type
{
};

// Check if a Function like this is implemented in the namespace of T
// (found by argument-dependent lookup):
//
// template <typename Func> std::string_view TypeDefinition(T&, Func&);

template <typename T, class = void>
struct has_TypeDefinitionADL : std::false_type
{
};

template <typename T>
struct has_TypeDefinitionADL<
    T, enable_if_same_t<std::string_view,
                        decltype(TypeDefinition(std::declval<T&>(),
                                                std::declval<EmptyFunc&>()))>>
  : std::true_type
{
};

// True if T is described either by DataTamer::TypeDefinitionTrait<T> or by
// a TypeDefinition() overload found by ADL.
// (The second parameter is unused; kept for source compatibility.)
template <typename T, class = void>
struct has_TypeDefinition : std::bool_constant<has_TypeDefinitionTrait<T>::value ||
                                               has_TypeDefinitionADL<T>::value>
{
  static_assert(!is_TypeDefinitionTrait_specialized<T>::value ||
                    has_TypeDefinitionTrait<T>::value,
                "DataTamer::TypeDefinitionTrait<T> is specialized, but it has no "
                "static define(T&, AddField&) callable with a generic AddField");
};

// Call the definition of T: DataTamer::TypeDefinitionTrait<T>::define() if
// specialized, otherwise the ADL overload TypeDefinition(obj, add_field).
// Every place that walks the fields of a custom type must go through this.
template <typename T, typename AddField>
inline decltype(auto) InvokeTypeDefinition(T& obj, AddField& add_field)
{
  static_assert(has_TypeDefinition<T>::value, "Missing TypeDefinition");
  if constexpr(has_TypeDefinitionTrait<T>::value)
  {
    return DataTamer::TypeDefinitionTrait<T>::define(obj, add_field);
  }
  else
  {
    return TypeDefinition(obj, add_field);
  }
}

//------------- Forward declarations of BufferSize ------------------

template <typename T, bool = true>
size_t BufferSize(const T& val);

template <>
size_t BufferSize(const std::string& str);

template <class T, size_t N,
          std::enable_if_t<!has_TypeDefinition<std::array<T, N>>::value, bool> = true>
size_t BufferSize(const std::array<T, N>& v);

template <
    template <class, class> class Container, class T, class... TArgs,
    std::enable_if_t<!has_TypeDefinition<Container<T, TArgs...>>::value, bool> = true>
size_t BufferSize(const Container<T, TArgs...>& vect);

//---------- Forward declarations of DeserializeFromBuffer -----------

template <typename T, bool = true>
void DeserializeFromBuffer(SpanBytesConst& buffer, T& dest);

template <>
void DeserializeFromBuffer(SpanBytesConst& buffer, std::string& str);

template <class T, size_t N,
          std::enable_if_t<!has_TypeDefinition<std::array<T, N>>::value, bool> = true>
void DeserializeFromBuffer(SpanBytesConst& buffer, std::array<T, N>& v);

template <
    template <class, class> class Container, class T, class... TArgs,
    std::enable_if_t<!has_TypeDefinition<Container<T, TArgs...>>::value, bool> = true>
void DeserializeFromBuffer(SpanBytesConst& buffer, Container<T, TArgs...>& dest);

//---------- Forward declarations of SerializeIntoBuffer -----------

template <typename T, bool = true>
void SerializeIntoBuffer(SpanBytes& buffer, const T& value);

template <>
void SerializeIntoBuffer(SpanBytes& buffer, const std::string& str);

template <class T, size_t N,
          std::enable_if_t<!has_TypeDefinition<std::array<T, N>>::value, bool> = true>
void SerializeIntoBuffer(SpanBytes& buffer, const std::array<T, N>& v);

template <
    template <class, class> class Container, class T, class... TArgs,
    std::enable_if_t<!has_TypeDefinition<Container<T, TArgs...>>::value, bool> = true>
void SerializeIntoBuffer(SpanBytes& buffer, const Container<T, TArgs...>& vect);

//-----------------------------------------------------------------------
//-----------------------------------------------------------------------
//-----------------------------------------------------------------------

template <typename T>
inline T const* Span<T>::data() const
{
  return data_;
}

template <typename T>
inline T* Span<T>::data()
{
  return data_;
}

template <typename T>
inline size_t Span<T>::size() const
{
  return size_;
}

template <typename T>
inline void Span<T>::trimFront(size_t offset)
{
  if(offset > size_)
  {
    throw std::runtime_error("Buffer overrun");
  }
  size_ -= offset;
  data_ += offset;
}

// The wire format uses a little endian encoding (since that's efficient for
// the common platforms).
#if defined(__s390x__)
#define SERIALIZE_LITTLEENDIAN 0
#endif  // __s390x__
#if !defined(SERIALIZE_LITTLEENDIAN)
#if defined(__GNUC__) || defined(__clang__) || defined(__ICCARM__)
#if (defined(__BIG_ENDIAN__) ||                                                          \
     (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__))
#define SERIALIZE_LITTLEENDIAN 0
#else
#define SERIALIZE_LITTLEENDIAN 1
#endif  // __BIG_ENDIAN__
#elif defined(_MSC_VER)
#if defined(_M_PPC)
#define SERIALIZE_LITTLEENDIAN 0
#else
#define SERIALIZE_LITTLEENDIAN 1
#endif
#else
#error Unable to determine endianness, define SERIALIZE_LITTLEENDIAN.
#endif
#endif  // !defined(SERIALIZE_LITTLEENDIAN)

template <typename T>
inline T EndianSwap(T t)
{
  static_assert(std::is_arithmetic<T>::value, "This function accepts only numeric types");
#if defined(_MSC_VER)
#define DESERIALIZE_ME_BYTESWAP16 _byteswap_ushort
#define DESERIALIZE_ME_BYTESWAP32 _byteswap_ulong
#define DESERIALIZE_ME_BYTESWAP64 _byteswap_uint64
#else
#if defined(__GNUC__) && __GNUC__ * 100 + __GNUC_MINOR__ < 408 && !defined(__clang__)
// __builtin_bswap16 was missing prior to GCC 4.8.
#define DESERIALIZE_ME_BYTESWAP16(x)                                                     \
  static_cast<uint16_t>(__builtin_bswap32(static_cast<uint32_t>(x) << 16))
#else
#define DESERIALIZE_ME_BYTESWAP16 __builtin_bswap16
#endif
#define DESERIALIZE_ME_BYTESWAP32 __builtin_bswap32
#define DESERIALIZE_ME_BYTESWAP64 __builtin_bswap64
#endif
  if constexpr(sizeof(T) == 1)
  {  // Compile-time if-then's.
    return t;
  }
  else if constexpr(sizeof(T) == 2)
  {
    union
    {
      T t;
      uint16_t i;
    } u;
    u.t = t;
    u.i = DESERIALIZE_ME_BYTESWAP16(u.i);
    return u.t;
  }
  else if constexpr(sizeof(T) == 4)
  {
    union
    {
      T t;
      uint32_t i;
    } u;
    u.t = t;
    u.i = DESERIALIZE_ME_BYTESWAP32(u.i);
    return u.t;
  }
  else if(sizeof(T) == 8)
  {
    union
    {
      T t;
      uint64_t i;
    } u;
    u.t = t;
    u.i = DESERIALIZE_ME_BYTESWAP64(u.i);
    return u.t;
  }
  else
  {
    std::runtime_error("Problem with IndianSwap");
  }
}
template <typename T>
inline constexpr bool is_number()
{
  return std::is_arithmetic_v<T> || std::is_same_v<T, std::byte> || std::is_enum_v<T>;
}

template <typename _Tp, bool _is_container, int _size>
struct container_info_
{
  static constexpr bool is_container = _is_container;
  static constexpr int size = _size;
  typedef _Tp value_type;
};

template <typename T>
struct container_info : container_info_<T, false, -1>
{
};

template <template <class, class> class Container, class T, class... TArgs>
struct container_info<Container<T, TArgs...>> : container_info_<T, true, 0>
{
};

template <typename T, size_t S>
struct container_info<std::array<T, S>> : container_info_<T, true, int(S)>
{
};

template <typename>
struct is_std_vector : std::false_type
{
};

template <typename T, typename... TArgs>
struct is_std_vector<std::vector<T, TArgs...>> : std::true_type
{
};

template <typename>
struct is_std_array : std::false_type
{
  const static size_t Size = 0;
};

template <typename T, size_t S>
struct is_std_array<std::array<T, S>> : std::true_type
{
  const static size_t Size = S;
};

template <typename T>
inline constexpr bool is_vector()
{
  return (is_std_vector<T>::value || is_std_array<T>::value);
}

//-----------------------------------------------------------------------
//-----------------------------------------------------------------------
//-----------------------------------------------------------------------

template <typename T, bool>
inline size_t BufferSize(const T& val)
{
  static_assert(is_number<T>() || has_TypeDefinition<T>(), "Missing TypeDefinition");

  if constexpr(is_number<T>())
  {
    return sizeof(T);
  }
  else
  {
    size_t total_size = 0;
    auto func = [&total_size](const char*, auto const* field) {
      total_size += BufferSize(*field);
    };

    InvokeTypeDefinition(const_cast<T&>(val), func);
    return total_size;
  }
}

template <>
inline size_t BufferSize(const std::string& str)
{
  return sizeof(StringSize) + str.size();
}

template <class T, size_t N,
          std::enable_if_t<!has_TypeDefinition<std::array<T, N>>::value, bool>>
inline size_t BufferSize(const std::array<T, N>&)
{
  return BufferSize(T{}) * N;
}

template <template <class, class> class Container, class T, class... TArgs,
          std::enable_if_t<!has_TypeDefinition<Container<T, TArgs...>>::value, bool>>
inline size_t BufferSize(const Container<T, TArgs...>& vect)
{
  if constexpr(std::is_trivially_copyable_v<T> && is_vector<Container<T, TArgs...>>())
  {
    return sizeof(uint32_t) + vect.size() * sizeof(T);
  }
  else
  {
    auto size = sizeof(uint32_t);
    for(const auto& v : vect)
    {
      size += BufferSize(v);
    }
    return size;
  }
}

//-----------------------------------------------------------------------
//-----------------------------------------------------------------------
//-----------------------------------------------------------------------

template <typename T, bool>
inline void DeserializeFromBuffer(SpanBytesConst& buffer, T& dest)
{
  static_assert(is_number<T>() || has_TypeDefinition<T>(), "Missing TypeDefinition");

  if constexpr(is_number<T>())
  {
    auto const S = sizeof(T);
    if(S > buffer.size())
    {
      throw std::runtime_error("DeserializeFromBuffer: buffer overflow");
    }
    std::memcpy(&dest, buffer.data(), S);  // buffer.data() may be misaligned for T

#if SERIALIZE_LITTLEENDIAN == 0
    dest = EndianSwap<T>(dest);
#endif
    buffer = SpanBytesConst(buffer.data() + S, buffer.size() - S);  // NOLINT
  }
  else
  {
    // fields are written: the pointer must not be const
    auto func = [&buffer](const char*, auto* field) {
      DeserializeFromBuffer(buffer, *field);
    };
    InvokeTypeDefinition(dest, func);
  }
}

template <>
inline void DeserializeFromBuffer(SpanBytesConst& buffer, std::string& dest)
{
  StringSize size = 0;
  DeserializeFromBuffer(buffer, size);

  if(size > buffer.size())
  {
    throw std::runtime_error("DeserializeFromBuffer: buffer overflow");
  }

  dest.assign(reinterpret_cast<char const*>(buffer.data()), size);
  buffer.trimFront(size);
}

template <typename T, size_t N,
          std::enable_if_t<!has_TypeDefinition<std::array<T, N>>::value, bool>>
inline void DeserializeFromBuffer(SpanBytesConst& buffer, std::array<T, N>& dest)
{
  if(N * BufferSize(T{}) > buffer.size())
  {
    throw std::runtime_error("DeserializeFromBuffer: buffer overflow");
  }

  if constexpr(sizeof(T) == 1)
  {
    memcpy(dest.data(), buffer.data(), N);
    buffer.trimFront(N);
  }
  else
  {
    for(size_t i = 0; i < N; i++)
    {
      DeserializeFromBuffer(buffer, dest[i]);
    }
  }
}

template <template <class, class> class Container, class T, class... TArgs,
          std::enable_if_t<!has_TypeDefinition<Container<T, TArgs...>>::value, bool>>
inline void DeserializeFromBuffer(SpanBytesConst& buffer, Container<T, TArgs...>& dest)
{
  uint32_t num_values = 0;
  DeserializeFromBuffer(buffer, num_values);

  // if the container offers contiguous memory, you can just use memcpy
  if constexpr(sizeof(T) == 1 && is_vector<Container<T, TArgs...>>())
  {
    if constexpr(container_info<Container<T, TArgs...>>::size == 0)
    {
      dest.resize(num_values);
    }
    else if constexpr(std::is_array_v<Container<T, TArgs...>>)
    {
      if(std::size(dest) != num_values)
      {
        throw std::runtime_error("DeserializeFromBuffer: wrong size in static container");
      }
    }

    const size_t size = num_values * BufferSize(T{});
    memcpy(dest.data(), buffer.data(), size);
    buffer.trimFront(size);
  }
  else
  {
    dest.clear();
    for(size_t i = 0; i < num_values; i++)
    {
      T temp;
      DeserializeFromBuffer(buffer, temp);
      std::back_inserter(dest) = std::move(temp);
    }
  }
}

//-----------------------------------------------------------------------
//-----------------------------------------------------------------------
//-----------------------------------------------------------------------

template <typename T, bool>
inline void SerializeIntoBuffer(SpanBytes& buffer, T const& value)
{
  static_assert(is_number<T>() || has_TypeDefinition<T>(), "Missing TypeDefinition");

  if constexpr(is_number<T>())
  {
    const size_t S = sizeof(T);
    if(S > buffer.size())
    {
      throw std::runtime_error("SerializeIntoBuffer: buffer overflow");
    }
#if SERIALIZE_LITTLEENDIAN == 0
    T swapped = EndianSwap<T>(value);
    std::memcpy(buffer.data(), &swapped, S);
#else
    std::memcpy(buffer.data(), &value, S);
#endif
    buffer.trimFront(S);  // NOLINT
  }
  else
  {
    auto func = [&buffer](const char*, const auto* field) {
      SerializeIntoBuffer(buffer, *field);
    };
    InvokeTypeDefinition(const_cast<T&>(value), func);
  }
}

template <>
inline void SerializeIntoBuffer(SpanBytes& buffer, std::string const& str)
{
  if(str.size() > std::numeric_limits<StringSize>::max())
  {
    throw std::runtime_error("SerializeIntoBuffer: string exceeds maximum size");
  }

  if((str.size() + sizeof(StringSize)) > buffer.size())
  {
    throw std::runtime_error("SerializeIntoBuffer: buffer overflow");
  }

  const auto size = static_cast<StringSize>(str.size());
  SerializeIntoBuffer(buffer, size);

  memcpy(buffer.data(), str.data(), size);
  buffer.trimFront(size);
}

template <typename T, size_t N,
          std::enable_if_t<!has_TypeDefinition<std::array<T, N>>::value, bool>>
inline void SerializeIntoBuffer(SpanBytes& buffer, std::array<T, N> const& vect)
{
  if(N > std::numeric_limits<uint32_t>::max())
  {
    throw std::runtime_error("SerializeIntoBuffer: array exceeds maximum size");
  }

  if constexpr(std::is_arithmetic_v<T> || std::is_same_v<T, std::byte>)
  {
    if(N * sizeof(T) > buffer.size())
    {
      throw std::runtime_error("SerializeIntoBuffer: buffer overflow");
    }
  }

  if constexpr(sizeof(T) == 1)
  {
    std::memcpy(buffer.data(), vect.data(), N);
    buffer.trimFront(N);
  }
  else
  {
    for(size_t i = 0; i < N; i++)
    {
      SerializeIntoBuffer(buffer, vect[i]);
    }
  }
}

template <template <class, class> class Container, class T, class... TArgs,
          std::enable_if_t<!has_TypeDefinition<Container<T, TArgs...>>::value, bool>>
inline void SerializeIntoBuffer(SpanBytes& buffer, Container<T, TArgs...> const& vect)
{
  const auto num_values = static_cast<uint32_t>(vect.size());
  SerializeIntoBuffer(buffer, num_values);

  // can use memcpy if the size of T is 1
  if constexpr(sizeof(T) == 1 && is_vector<Container<T, TArgs...>>())
  {
    const size_t size = num_values;
    if(size > buffer.size())
    {
      throw std::runtime_error("SerializeIntoBuffer: buffer overflow");
    }
    memcpy(buffer.data(), vect.data(), size);
    buffer.trimFront(size);
  }
  else
  {
    for(const T& v : vect)
    {
      SerializeIntoBuffer(buffer, v);
    }
  }
}

}  // namespace SerializeMe
