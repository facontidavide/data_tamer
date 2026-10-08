// Built with SERIALIZE_LITTLEENDIAN=0, as for a big endian host (see CMakeLists.txt):
// every number is byte-swapped on its way in and out of the buffer. On a little endian
// machine that makes the "wire" bytes the reverse of the memory bytes, which is what a
// big endian host produces for the little endian wire format.
#include "data_tamer/contrib/SerializeMe.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

static_assert(SERIALIZE_LITTLEENDIAN == 0, "this file tests the byte-swapping path");

using namespace SerializeMe;

namespace
{
enum class Mode16 : uint16_t
{
  Value = 0x0102
};

template <typename T>
std::vector<uint8_t> memoryBytes(const T& value)
{
  std::vector<uint8_t> bytes(sizeof(T));
  std::memcpy(bytes.data(), &value, sizeof(T));
  return bytes;
}

template <typename T>
std::vector<uint8_t> reversed(const T& value)
{
  auto bytes = memoryBytes(value);
  std::reverse(bytes.begin(), bytes.end());
  return bytes;
}

template <typename T>
std::vector<uint8_t> serialized(const T& value)
{
  std::vector<uint8_t> storage(BufferSize(value));
  SpanBytes out(storage);
  SerializeIntoBuffer(out, value);
  return storage;
}
}  // namespace

TEST(SerializeMeBigEndian, EndianSwapReversesTheBytesOfEveryWidth)
{
  EXPECT_EQ(EndianSwap<uint8_t>(0xAB), 0xAB);
  EXPECT_EQ(EndianSwap<uint16_t>(0x0102), 0x0201);
  EXPECT_EQ(EndianSwap<int32_t>(0x01020304), 0x04030201);
  EXPECT_EQ(EndianSwap<uint64_t>(0x0102030405060708ULL), 0x0807060504030201ULL);

  const double value = 1.5;
  EXPECT_EQ(memoryBytes(EndianSwap(value)), reversed(value));
  const float single = -2.25F;
  EXPECT_EQ(memoryBytes(EndianSwap(single)), reversed(single));
}

TEST(SerializeMeBigEndian, EnumsAndBytesAreSwappedLikeTheirUnderlyingType)
{
  EXPECT_EQ(static_cast<uint16_t>(EndianSwap(Mode16::Value)), 0x0201);
  EXPECT_EQ(EndianSwap(std::byte{ 0xAB }), std::byte{ 0xAB });
}

TEST(SerializeMeBigEndian, NumbersAreWrittenWithTheirBytesReversed)
{
  EXPECT_EQ(serialized(uint32_t{ 0x01020304 }), reversed(uint32_t{ 0x01020304 }));
  EXPECT_EQ(serialized(uint64_t{ 0x0102030405060708ULL }),
            reversed(uint64_t{ 0x0102030405060708ULL }));
  EXPECT_EQ(serialized(-2.5), reversed(-2.5));
  EXPECT_EQ(serialized(Mode16::Value), reversed(Mode16::Value));
  EXPECT_EQ(serialized(true), (std::vector<uint8_t>{ 1 }));
}

TEST(SerializeMeBigEndian, ValuesRoundTrip)
{
  const std::vector<uint32_t> values = { 1, 0x01020304, 0xffffffff };
  const auto bytes = serialized(values);
  // the count is a number too: its bytes are reversed like the elements'
  EXPECT_EQ(std::vector<uint8_t>(bytes.begin(), bytes.begin() + 4),
            reversed(uint32_t{ 3 }));

  SpanBytesConst in(bytes);
  std::vector<uint32_t> decoded;
  DeserializeFromBuffer(in, decoded);
  EXPECT_EQ(decoded, values);
  EXPECT_EQ(in.size(), 0u);

  const std::array<Mode16, 2> modes = { Mode16::Value, Mode16{ 7 } };
  const auto mode_bytes = serialized(modes);
  SpanBytesConst mode_in(mode_bytes);
  std::array<Mode16, 2> decoded_modes{};
  DeserializeFromBuffer(mode_in, decoded_modes);
  EXPECT_EQ(decoded_modes, modes);
}
