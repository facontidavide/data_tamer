#include "data_tamer/channel.hpp"
#include "data_tamer/contrib/SerializeMe.hpp"
#include "data_tamer/custom_types.hpp"
#include "data_tamer/sinks/dummy_sink.hpp"
#include "test_sinks.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

// BufferSize() must be the number of bytes SerializeIntoBuffer() writes: the channel
// sizes each snapshot with the first and serializes with the second.

namespace
{
struct Blob
{
  std::vector<uint8_t> bytes;
};
template <typename AddField>
std::string_view TypeDefinition(Blob& p, AddField& add)
{
  add("bytes", &p.bytes);
  return "Blob";
}

struct Pair
{
  std::array<Blob, 2> blobs;
  uint32_t tail = 7;
};
template <typename AddField>
std::string_view TypeDefinition(Pair& p, AddField& add)
{
  add("blobs", &p.blobs);
  add("tail", &p.tail);
  return "Pair";
}

struct Point2
{
  double x = 0;
  double y = 0;
};
template <typename AddField>
std::string_view TypeDefinition(Point2& p, AddField& add)
{
  add("x", &p.x);
  add("y", &p.y);
  return "Point2";
}

Pair makePair()
{
  Pair pair;
  pair.blobs[0].bytes = std::vector<uint8_t>(100, 1);
  pair.blobs[1].bytes = std::vector<uint8_t>(50, 2);
  return pair;
}

// 2 counts of 4 bytes, 150 bytes of data and the 4 byte tail
constexpr size_t kPairBytes = 4 + 100 + 4 + 50 + 4;
}  // namespace

TEST(SerializeMeSize, ArrayOfVariableSizeElementsIsSizedFromItsElements)
{
  const auto pair = makePair();
  EXPECT_EQ(SerializeMe::BufferSize(pair.blobs), size_t{ 4 + 100 + 4 + 50 });
  EXPECT_EQ(SerializeMe::BufferSize(pair), kPairBytes);
}

TEST(SerializeMeSize, ArrayOfFixedSizeElementsKeepsItsSize)
{
  EXPECT_EQ(SerializeMe::BufferSize(std::array<uint16_t, 5>{}), size_t{ 10 });
  EXPECT_EQ(SerializeMe::BufferSize(std::array<Point2, 3>{}), size_t{ 48 });
}

TEST(SerializeMeSize, SnapshotOfAnArrayOfVariableSizeElementsFitsItsSlot)
{
  auto channel = DataTamer::LogChannel::create("sizes");
  DataTamerTest::Attached<DataTamer::DummySink> sink;
  auto pair = makePair();
  channel->registerValue("pair", &pair);
  channel->addDataSink(sink);
  channel->startLogging();

  EXPECT_EQ(channel->takeSnapshot(), DataTamer::SnapshotResult::ok);
  sink.drain();
  EXPECT_EQ(sink->latestPayloadSize(), kPairBytes);
}

TEST(SerializeMeSize, RealTimeSnapshotOfAnArrayOfVariableSizeElementsDoesNotThrow)
{
  auto channel = DataTamer::LogChannel::create("sizes");
  DataTamerTest::Attached<DataTamer::DummySink> sink;
  auto pair = makePair();
  channel->registerValue("pair", &pair);
  channel->addDataSink(sink);
  channel->startLogging();

  DataTamer::SnapshotResult result = DataTamer::SnapshotResult::rejected;
  EXPECT_NO_THROW(result = channel->tryTakeSnapshot());
  EXPECT_EQ(result, DataTamer::SnapshotResult::ok);
}
