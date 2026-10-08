#include "data_tamer/contrib/SerializeMe.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "guarded_buffer.hpp"

using namespace SerializeMe;
using DataTamerTest::GuardedBuffer;

// Every buffer ends at an inaccessible page: a read or write past its end faults
// instead of passing unnoticed.

TEST(SerializeMeBounds, ByteVectorRejectsACountLargerThanTheBytesThatFollow)
{
  GuardedBuffer buffer({ 0xff, 0xff, 0x00, 0x00, 1, 2, 3, 4 });  // count 65535, 4 bytes
  SpanBytesConst in(buffer.data(), buffer.size());
  std::vector<uint8_t> out;
  EXPECT_THROW(DeserializeFromBuffer(in, out), std::runtime_error);
  EXPECT_TRUE(out.empty());
}

TEST(SerializeMeBounds, ByteVectorRejectsACountOneByteTooLarge)
{
  GuardedBuffer buffer({ 4, 0, 0, 0, 1, 2, 3 });
  SpanBytesConst in(buffer.data(), buffer.size());
  std::vector<uint8_t> out;
  EXPECT_THROW(DeserializeFromBuffer(in, out), std::runtime_error);
}

TEST(SerializeMeBounds, ByteVectorReadsExactlyTheBytesThatFollow)
{
  GuardedBuffer buffer({ 3, 0, 0, 0, 1, 2, 3 });
  SpanBytesConst in(buffer.data(), buffer.size());
  std::vector<uint8_t> out;
  DeserializeFromBuffer(in, out);
  EXPECT_EQ(out, (std::vector<uint8_t>{ 1, 2, 3 }));
  EXPECT_EQ(in.size(), 0u);
}

namespace
{
enum class Flag8 : uint8_t
{
  A = 1
};

struct OneByte
{
  uint8_t v = 7;
};
template <typename AddField>
std::string_view TypeDefinition(OneByte& p, AddField& add)
{
  add("v", &p.v);
  return "OneByte";
}
}  // namespace

TEST(SerializeMeBounds, ByteEnumArrayDoesNotWritePastTheBuffer)
{
  std::array<Flag8, 16> values;
  values.fill(Flag8::A);
  GuardedBuffer buffer(8);
  SpanBytes out(buffer.data(), buffer.size());
  EXPECT_THROW(SerializeIntoBuffer(out, values), std::runtime_error);
}

TEST(SerializeMeBounds, OneByteStructArrayDoesNotWritePastTheBuffer)
{
  std::array<OneByte, 16> values;
  GuardedBuffer buffer(8);
  SpanBytes out(buffer.data(), buffer.size());
  EXPECT_THROW(SerializeIntoBuffer(out, values), std::runtime_error);
}

TEST(SerializeMeBounds, ByteEnumArrayFillsAnExactlySizedBuffer)
{
  std::array<Flag8, 8> values;
  values.fill(Flag8::A);
  GuardedBuffer buffer(8);
  SpanBytes out(buffer.data(), buffer.size());
  SerializeIntoBuffer(out, values);
  EXPECT_EQ(out.size(), 0u);
  EXPECT_EQ(std::vector<uint8_t>(buffer.data(), buffer.data() + 8),
            std::vector<uint8_t>(8, 1));
}

namespace
{
// The byte a bool object holds; reading it through memcpy is valid for any value.
uint8_t representation(const bool& value)
{
  uint8_t raw = 0;
  std::memcpy(&raw, &value, 1);
  return raw;
}
}  // namespace

// A byte other than 0 or 1 must not turn into a bool object that is neither.

TEST(SerializeMeCorruptInput, BoolReadFromANonZeroByteIsTrue)
{
  for(const uint8_t byte : { 0, 1, 2, 0x80, 0xff })
  {
    const std::vector<uint8_t> bytes{ byte };
    SpanBytesConst in(bytes);
    bool value = false;
    DeserializeFromBuffer(in, value);
    EXPECT_EQ(representation(value), byte == 0 ? 0 : 1) << "byte " << int(byte);
  }
}

TEST(SerializeMeCorruptInput, BoolArrayElementsAreValidBools)
{
  const std::vector<uint8_t> bytes{ 2, 0, 1, 0xff };
  SpanBytesConst in(bytes);
  std::array<bool, 4> values{};
  DeserializeFromBuffer(in, values);
  const std::vector<uint8_t> raw{ representation(values[0]), representation(values[1]),
                                  representation(values[2]), representation(values[3]) };
  EXPECT_EQ(raw, (std::vector<uint8_t>{ 1, 0, 1, 1 }));
}

namespace
{
// A container that claims 2^32 elements without holding any.
template <class T, class Unused = void>
struct HugeContainer
{
  size_t size() const { return size_t{ 1 } << 32; }
  const T* begin() const { return nullptr; }
  const T* end() const { return nullptr; }
};
}  // namespace

TEST(SerializeMeBounds, ContainerWithTooManyElementsForTheCountIsRejected)
{
  if constexpr(sizeof(size_t) <= sizeof(uint32_t))
  {
    GTEST_SKIP() << "size_t has 32 bits";
  }
  else
  {
    std::vector<uint8_t> storage(64);
    SpanBytes out(storage);
    EXPECT_THROW(SerializeIntoBuffer(out, HugeContainer<uint16_t>{}), std::runtime_error);
    EXPECT_EQ(out.size(), storage.size()) << "the truncated count 0 was written";
  }
}
