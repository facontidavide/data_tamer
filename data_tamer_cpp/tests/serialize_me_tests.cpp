#include "data_tamer/contrib/SerializeMe.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <type_traits>
#include <vector>

using namespace SerializeMe;

// Issue #73: the sizeof(T) == 1 branch copied in the wrong direction and did not
// even compile (const source used as destination).
TEST(SerializeMe, ByteArrayIsSerializedIntoTheBuffer)
{
  const std::array<uint8_t, 4> values = { 1, 2, 3, 4 };
  std::vector<uint8_t> storage(BufferSize(values), 0xEE);
  SpanBytes buffer(storage);
  SerializeIntoBuffer(buffer, values);
  ASSERT_EQ(buffer.size(), 0u);
  ASSERT_EQ(storage, (std::vector<uint8_t>{ 1, 2, 3, 4 }));

  std::array<uint8_t, 4> decoded{};
  SpanBytesConst input(storage.data(), storage.size());
  DeserializeFromBuffer(input, decoded);
  ASSERT_EQ(decoded, values);
}

// Issue #42: numbers were read through a reinterpret_cast, which is undefined
// behaviour at unaligned offsets. A leading byte forces every following field
// off its natural alignment; UBSAN fails this test with the old code.
TEST(SerializeMe, NumbersRoundTripAtUnalignedOffsets)
{
  const uint8_t tag = 7;
  const double d = 3.25;
  const int32_t i = -42;
  const uint64_t u = 0x0102030405060708ULL;
  std::vector<uint8_t> storage(BufferSize(tag) + BufferSize(d) + BufferSize(i) +
                               BufferSize(u));
  SpanBytes buffer(storage);
  SerializeIntoBuffer(buffer, tag);
  SerializeIntoBuffer(buffer, d);
  SerializeIntoBuffer(buffer, i);
  SerializeIntoBuffer(buffer, u);
  ASSERT_EQ(buffer.size(), 0u);

  SpanBytesConst input(storage.data(), storage.size());
  uint8_t tag_out = 0;
  double d_out = 0;
  int32_t i_out = 0;
  uint64_t u_out = 0;
  DeserializeFromBuffer(input, tag_out);
  DeserializeFromBuffer(input, d_out);
  DeserializeFromBuffer(input, i_out);
  DeserializeFromBuffer(input, u_out);
  ASSERT_EQ(tag_out, tag);
  ASSERT_EQ(d_out, d);
  ASSERT_EQ(i_out, i);
  ASSERT_EQ(u_out, u);
  ASSERT_EQ(input.size(), 0u);
}

// A read-only span views a const vector; a mutable span still cannot.
TEST(SerializeMe, ConstSpanFromConstVector)
{
  const std::vector<uint8_t> bytes = { 7, 8, 9 };
  const SpanBytesConst view = bytes;
  EXPECT_EQ(view.data(), bytes.data());
  EXPECT_EQ(view.size(), 3u);
  static_assert(!std::is_constructible_v<SpanBytes, const std::vector<uint8_t>&>);
  static_assert(std::is_constructible_v<SpanBytes, std::vector<uint8_t>&>);
}
