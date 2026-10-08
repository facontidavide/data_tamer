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
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace SerializeMe
{

// Minimal non-owning view of contiguous memory (a subset of std::span).
template <typename T>
class Span
{
public:
  Span() = default;

  Span(T* ptr, size_t size) : data_(ptr), size_(size) {}

  template <size_t N>
  Span(std::array<T, N>& v) : data_(v.data()), size_(N)
  {}

  // A template, so that Span<const T> never forms the ill-formed std::vector<const T>&.
  template <typename U, std::enable_if_t<std::is_same_v<U, T>, int> = 0>
  Span(std::vector<U>& v) : data_(v.data()), size_(v.size())
  {}

  /// Read-only view of a vector: Span<const uint8_t> from a (const) std::vector<uint8_t>.
  /// Like std::span it also binds to a temporary: do not keep the span past it.
  template <typename U, std::enable_if_t<std::is_same_v<const U, T>, int> = 0>
  Span(const std::vector<U>& v) : data_(v.data()), size_(v.size())
  {}

  /// Read-only view of a (const) std::array<uint8_t, N>, as for a vector.
  template <typename U, size_t N, std::enable_if_t<std::is_same_v<const U, T>, int> = 0>
  Span(const std::array<U, N>& v) : data_(v.data()), size_(N)
  {}

  T const* data() const;

  T* data();

  size_t size() const;

  /// Drops the first `offset` elements; throws std::runtime_error if there are fewer.
  void trimFront(size_t offset);

private:
  T* data_ = nullptr;
  size_t size_ = 0;
};

using SpanBytes = Span<uint8_t>;
using SpanBytesConst = Span<uint8_t const>;
using StringSize = uint32_t;

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
 * Specialize it in namespace DataTamer. define() calls add("name", &member) for each
 * field and returns the type name:
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
 * - The name is a view of static storage (a string literal) or an owning std::string,
 *   which is evaluated once per type and cached.
 * - An optional `static std::string name()` builds the name at runtime (class
 *   templates); define()'s return value is then ignored and may be void.
 * - The second template parameter allows partial specializations (std::enable_if).
 * - The trait wins over an ADL TypeDefinition(). A define() that cannot be called as
 *   define(T&, AddField&) is a compile error.
 * - The specialization must be visible before the type is first used.
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

// True if DataTamer::TypeDefinitionTrait<T> has a static define(T&, AddField&).
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

// True if a TypeDefinition() overload is found for T by argument-dependent lookup. It
// returns the type name as a std::string_view, a const char* or a std::string:
//
// template <typename Func> std::string_view TypeDefinition(T&, Func&);

template <typename T, class = void>
struct has_TypeDefinitionADL : std::false_type
{
};

template <typename T>
struct has_TypeDefinitionADL<
    T, std::enable_if_t<std::is_convertible_v<
           decltype(TypeDefinition(std::declval<T&>(), std::declval<EmptyFunc&>())),
           std::string_view>>> : std::true_type
{
};

// True if T is described by DataTamer::TypeDefinitionTrait<T> or by an ADL
// TypeDefinition() overload. The second parameter is unused (source compatibility).
template <typename T, class = void>
struct has_TypeDefinition : std::bool_constant<has_TypeDefinitionTrait<T>::value ||
                                               has_TypeDefinitionADL<T>::value>
{
  static_assert(!is_TypeDefinitionTrait_specialized<T>::value ||
                    has_TypeDefinitionTrait<T>::value,
                "DataTamer::TypeDefinitionTrait<T> is specialized, but it has no "
                "static define(T&, AddField&) callable with a generic AddField");
};

// Calls the definition of T: the trait's define() if specialized, otherwise the ADL
// TypeDefinition(). Every walk over the fields of a custom type must go through it.
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

/// Number of bytes SerializeIntoBuffer() writes for `val`.
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

/// Reads `dest` from the front of `buffer` and advances the span past the bytes read.
/// Throws std::runtime_error if the buffer is too short.
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

/// Writes `value` at the front of `buffer` and advances the span past the bytes written
/// (encoding: docs/wire_format.md). Throws std::runtime_error if the buffer is too small.
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

// The wire format is little endian (docs/wire_format.md); big endian hosts byte-swap.
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
inline constexpr bool is_number()
{
  return std::is_arithmetic_v<T> || std::is_same_v<T, std::byte> || std::is_enum_v<T>;
}

template <typename T>
inline T EndianSwap(T t)
{
  static_assert(is_number<T>(), "This function accepts only numeric types");
  static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8,
                "This function accepts only 1, 2, 4 and 8 byte types");
#if defined(_MSC_VER)
#define DESERIALIZE_ME_BYTESWAP16 _byteswap_ushort
#define DESERIALIZE_ME_BYTESWAP32 _byteswap_ulong
#define DESERIALIZE_ME_BYTESWAP64 _byteswap_uint64
#else
#if defined(__GNUC__) && __GNUC__ * 100 + __GNUC_MINOR__ < 408 && !defined(__clang__)
// GCC before 4.8 has no __builtin_bswap16.
#define DESERIALIZE_ME_BYTESWAP16(x)                                                     \
  static_cast<uint16_t>(__builtin_bswap32(static_cast<uint32_t>(x) << 16))
#else
#define DESERIALIZE_ME_BYTESWAP16 __builtin_bswap16
#endif
#define DESERIALIZE_ME_BYTESWAP32 __builtin_bswap32
#define DESERIALIZE_ME_BYTESWAP64 __builtin_bswap64
#endif
  if constexpr(sizeof(T) == 1)
  {
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
  else
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
}
#undef DESERIALIZE_ME_BYTESWAP16
#undef DESERIALIZE_ME_BYTESWAP32
#undef DESERIALIZE_ME_BYTESWAP64

template <typename _Tp, bool _is_container, int _size>
struct container_info_
{
  static constexpr bool is_container = _is_container;
  static constexpr int size = _size;
  typedef _Tp value_type;
};

// A type with a TypeDefinition is a custom type, whatever its template shape.
template <typename T, typename = void>
struct container_info : container_info_<T, false, -1>
{
};

template <template <class, class> class Container, class T, class... TArgs>
struct container_info<
    Container<T, TArgs...>,
    std::enable_if_t<!has_TypeDefinition<Container<T, TArgs...>>::value>>
  : container_info_<T, true, 0>
{
};

template <typename T, size_t S>
struct container_info<std::array<T, S>,
                      std::enable_if_t<!has_TypeDefinition<std::array<T, S>>::value>>
  : container_info_<T, true, int(S)>
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
inline size_t BufferSize([[maybe_unused]] const std::array<T, N>& vect)
{
  if constexpr(is_number<T>())
  {
    return sizeof(T) * N;
  }
  else
  {
    size_t total = 0;
    for(const auto& v : vect)
    {
      total += BufferSize(v);
    }
    return total;
  }
}

template <template <class, class> class Container, class T, class... TArgs,
          std::enable_if_t<!has_TypeDefinition<Container<T, TArgs...>>::value, bool>>
inline size_t BufferSize(const Container<T, TArgs...>& vect)
{
  if constexpr(is_number<T>() && is_vector<Container<T, TArgs...>>())
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
    if constexpr(std::is_same_v<T, bool>)
    {
      dest = buffer.data()[0] != 0;  // another byte value is not a valid bool
    }
    else
    {
      std::memcpy(&dest, buffer.data(), S);  // buffer.data() may be misaligned for T

#if SERIALIZE_LITTLEENDIAN == 0
      dest = EndianSwap<T>(dest);
#endif
    }
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

  if constexpr(sizeof(T) == 1 && !std::is_same_v<T, bool>)
  {
    std::memcpy(dest.data(), buffer.data(), N);
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

  // contiguous 1-byte elements: one memcpy
  if constexpr(sizeof(T) == 1 && is_vector<Container<T, TArgs...>>())
  {
    const size_t size = num_values * BufferSize(T{});
    if(size > buffer.size())
    {
      throw std::runtime_error("DeserializeFromBuffer: buffer overflow");
    }

    dest.resize(num_values);
    std::memcpy(dest.data(), buffer.data(), size);
    buffer.trimFront(size);
  }
  else
  {
    dest.clear();
    for(size_t i = 0; i < num_values; i++)
    {
      T temp;
      DeserializeFromBuffer(buffer, temp);
      dest.push_back(std::move(temp));
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
  if constexpr(sizeof(size_t) > sizeof(StringSize))
  {
    if(str.size() > std::numeric_limits<StringSize>::max())
    {
      throw std::runtime_error("SerializeIntoBuffer: string exceeds maximum size");
    }
  }

  if((str.size() + sizeof(StringSize)) > buffer.size())
  {
    throw std::runtime_error("SerializeIntoBuffer: buffer overflow");
  }

  const auto size = static_cast<StringSize>(str.size());
  SerializeIntoBuffer(buffer, size);

  std::memcpy(buffer.data(), str.data(), size);
  buffer.trimFront(size);
}

template <typename T, size_t N,
          std::enable_if_t<!has_TypeDefinition<std::array<T, N>>::value, bool>>
inline void SerializeIntoBuffer(SpanBytes& buffer, std::array<T, N> const& vect)
{
  if constexpr(is_number<T>() || sizeof(T) == 1)
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
  if constexpr(sizeof(size_t) > sizeof(uint32_t))
  {
    if(vect.size() > std::numeric_limits<uint32_t>::max())
    {
      throw std::runtime_error("SerializeIntoBuffer: container exceeds maximum size");
    }
  }
  const auto num_values = static_cast<uint32_t>(vect.size());
  SerializeIntoBuffer(buffer, num_values);

  // contiguous 1-byte elements: one memcpy
  if constexpr(sizeof(T) == 1 && is_vector<Container<T, TArgs...>>())
  {
    const size_t size = num_values;
    if(size > buffer.size())
    {
      throw std::runtime_error("SerializeIntoBuffer: buffer overflow");
    }
    std::memcpy(buffer.data(), vect.data(), size);
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
