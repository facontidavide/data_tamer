#include "data_tamer/values.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace DataTamer;

namespace
{
using Bytes = std::vector<uint8_t>;

enum class Small : uint8_t
{
  Two = 2
};

// Serializes into exactly the room the value needs, and returns what was written.
Bytes serializeExactly(const ValuePtr& ptr)
{
  Bytes memory(ptr.getSerializedSize());
  SerializeMe::SpanBytes room(memory);
  ptr.serialize(room);
  EXPECT_EQ(room.size(), 0u) << "the whole room should have been used";
  return memory;
}

// A value that does not fit into `room` bytes is refused, and nothing is written.
void expectRefused(const ValuePtr& ptr, size_t room)
{
  // Bytes past the room, and the room itself, keep their pattern.
  Bytes memory(room + 8, 0xAA);
  SerializeMe::SpanBytes span(memory.data(), room);
  EXPECT_THROW(ptr.serialize(span), std::runtime_error);
  EXPECT_EQ(span.size(), room);
  EXPECT_EQ(memory, Bytes(room + 8, 0xAA));

  // The room is the whole allocation: AddressSanitizer sees a write past it.
  auto tight = std::make_unique<uint8_t[]>(room);
  SerializeMe::SpanBytes tight_span(tight.get(), room);
  EXPECT_THROW(ptr.serialize(tight_span), std::runtime_error);
}
}  // namespace

// The expected bytes are the little-endian encoding of the wire format.
TEST(ValuePtrBounds, ANumberFillsExactlyItsRoomInLittleEndian)
{
  const int16_t i16 = -2;
  const uint32_t u32 = 0x01020304;
  const double f64 = 1.5;
  const bool flag = true;
  const Small small = Small::Two;
  const std::atomic<int32_t> atomic{ 7 };

  EXPECT_EQ(serializeExactly(ValuePtr(&i16)), (Bytes{ 0xFE, 0xFF }));
  EXPECT_EQ(serializeExactly(ValuePtr(&u32)), (Bytes{ 4, 3, 2, 1 }));
  EXPECT_EQ(serializeExactly(ValuePtr(&f64)), (Bytes{ 0, 0, 0, 0, 0, 0, 0xF8, 0x3F }));
  EXPECT_EQ(serializeExactly(ValuePtr(&flag)), (Bytes{ 1 }));
  EXPECT_EQ(serializeExactly(ValuePtr(&small)), (Bytes{ 2 }));
  EXPECT_EQ(serializeExactly(ValuePtr(&atomic)), (Bytes{ 7, 0, 0, 0 }));
}

// serialize() used to copy the bytes first and only then notice that they did not fit.
TEST(ValuePtrBounds, ANumberThatDoesNotFitIsRefusedBeforeItIsWritten)
{
  const int16_t i16 = -2;
  const double f64 = 1.5;
  const bool flag = true;
  const Small small = Small::Two;

  expectRefused(ValuePtr(&i16), 1);
  expectRefused(ValuePtr(&f64), 7);
  expectRefused(ValuePtr(&f64), 0);
  expectRefused(ValuePtr(&flag), 0);
  expectRefused(ValuePtr(&small), 0);
}

TEST(ValuePtrBounds, AnAtomicThatDoesNotFitIsRefusedBeforeItIsWritten)
{
  const std::atomic<int32_t> atomic{ 7 };
  const std::atomic<double> real{ 1.5 };

  expectRefused(ValuePtr(&atomic), 3);
  expectRefused(ValuePtr(&real), 0);
}

// Only a numeric value can be serialized without a CustomSerializer.
TEST(ValuePtrBounds, ACustomTypeWithoutASerializerIsRefused)
{
  struct Opaque
  {
    int32_t a = 1;
    int32_t b = 2;
  };
  const Opaque opaque;

  EXPECT_THROW(ValuePtr ptr(&opaque), std::invalid_argument);
}
