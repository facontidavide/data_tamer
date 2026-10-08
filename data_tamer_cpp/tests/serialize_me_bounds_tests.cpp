#include "data_tamer/contrib/SerializeMe.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
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
