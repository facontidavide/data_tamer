#include "data_tamer/channel.hpp"

#include "test_sinks.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace DataTamer;

namespace
{
// 16 bytes in memory (7 of padding), 9 on the wire: the format has no padding.
struct Padded
{
  char tag = 0;
  double value = 0;
};
template <typename AddField>
std::string_view TypeDefinition(Padded& item, AddField& add)
{
  add("tag", &item.tag);
  add("value", &item.value);
  return "Padded";
}
}  // namespace

// A payload that fits the slot is not `oversize`: the size the channel computes for a
// vector of structs counts the bytes written, not the bytes in memory.
TEST(PayloadEstimate, VectorOfPaddedStructsFitsASlotOfExactlyItsSize)
{
  DataTamerTest::Recording recording;
  const auto& channel = recording.channel;
  std::vector<Padded> items(2);
  channel->registerValue("items", &items);

  const size_t payload = 4 + 100 * 9;  // length prefix and 100 items
  channel->setPayloadCapacity(payload);
  channel->startLogging();
  items.resize(100);

  EXPECT_EQ(channel->tryTakeSnapshot(), SnapshotResult::ok);
  recording.sink.drain();
  EXPECT_EQ(recording.sink->latestPayloadSize(), payload);
  EXPECT_EQ(channel->stats().dropped_oversize, 0u);
}

// The bytes are the format's: the length, then each item with no padding.
TEST(PayloadEstimate, VectorOfStructsIsWrittenWithoutPadding)
{
  DataTamerTest::Recording recording;
  std::vector<Padded> items = { Padded{ 'x', 1.5 }, Padded{ 'y', -2.0 } };
  recording.channel->registerValue("items", &items);

  // The count (little endian), then 'x' and 1.5, then 'y' and -2.0.
  const double first = 1.5;
  const double second = -2.0;
  std::array<uint8_t, 4 + 9 + 9> expected = {};
  expected[0] = 2;
  expected[4] = 'x';
  std::memcpy(&expected[5], &first, sizeof(first));
  expected[13] = 'y';
  std::memcpy(&expected[14], &second, sizeof(second));
  EXPECT_EQ(std::vector<uint8_t>(expected.begin(), expected.end()), recording.payload());
}
