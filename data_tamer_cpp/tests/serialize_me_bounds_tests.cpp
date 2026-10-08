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
