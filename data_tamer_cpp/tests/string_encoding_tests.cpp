#include "data_tamer/channel.hpp"
#include "data_tamer/contrib/SerializeMe.hpp"
#include "data_tamer/custom_types.hpp"
#include "decode_utils.hpp"
#include "guarded_buffer.hpp"
#include "test_sinks.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// The schema describes a std::string as `char[]`: a uint32 count and that many chars
// (docs/wire_format.md). The payload must hold exactly that.

namespace
{
struct Named
{
  uint8_t tag = 1;
  std::string name;
};
template <typename AddField>
std::string_view TypeDefinition(Named& p, AddField& add)
{
  add("tag", &p.tag);
  add("name", &p.name);
  return "Named";
}
}  // namespace

TEST(StringEncoding, StringMemberIsSerializedAsACharVector)
{
  DataTamerTest::Recording recording;
  Named named;
  named.name = "abc";
  recording.channel->registerValue("n", &named);

  const auto snapshot = recording.snapshot();
  EXPECT_EQ(snapshot.payload, (std::vector<uint8_t>{ 1, 3, 0, 0, 0, 'a', 'b', 'c' }));
  EXPECT_NE(DataTamer::ToStr(recording.channel->getSchema()).find("char[] name\n"),
            std::string::npos);
  const std::map<std::string, double> expected = {
    { "n/tag", 1 }, { "n/name[0]", 'a' }, { "n/name[1]", 'b' }, { "n/name[2]", 'c' }
  };
  EXPECT_EQ(DataTamerTest::decode(*recording.channel, snapshot), expected);
}

TEST(StringEncoding, RegisteredStringIsSerializedAsACharVector)
{
  DataTamerTest::Recording recording;
  std::string text = "abc";
  recording.channel->registerValue("s", &text);

  EXPECT_EQ(recording.payload(), (std::vector<uint8_t>{ 3, 0, 0, 0, 'a', 'b', 'c' }));
}

TEST(StringEncoding, StringLongerThan64KiBIsSerialized)
{
  DataTamerTest::Recording recording;
  Named named;
  named.name = std::string(70 * 1024, 'x');
  recording.channel->registerValue("n", &named);
  recording.channel->startLogging();

  DataTamer::SnapshotResult result = DataTamer::SnapshotResult::rejected;
  EXPECT_NO_THROW(result = recording.channel->tryTakeSnapshot());
  EXPECT_EQ(result, DataTamer::SnapshotResult::ok);
  recording.sink.drain();
  EXPECT_EQ(recording.sink->latestPayloadSize(), size_t{ 1 + 4 + 70 * 1024 });
}

TEST(StringEncoding, StringThatGrowsPast64KiBIsSerialized)
{
  DataTamerTest::Recording recording;
  Named named;
  named.name = "abc";
  recording.channel->registerValue("n", &named);
  recording.channel->startLogging();

  named.name = std::string(70 * 1024, 'x');
  DataTamer::SnapshotResult result = DataTamer::SnapshotResult::rejected;
  EXPECT_NO_THROW(result = recording.channel->takeSnapshot());
  EXPECT_EQ(result, DataTamer::SnapshotResult::ok);
  recording.sink.drain();
  EXPECT_EQ(recording.sink->latestPayloadSize(), size_t{ 1 + 4 + 70 * 1024 });
}

TEST(StringEncoding, SerializeMeSizesAndRoundTripsAString)
{
  const std::string text = "hello";
  EXPECT_EQ(SerializeMe::BufferSize(text), size_t{ 4 + 5 });

  std::vector<uint8_t> storage(SerializeMe::BufferSize(text));
  SerializeMe::SpanBytes out(storage);
  SerializeMe::SerializeIntoBuffer(out, text);
  EXPECT_EQ(storage, (std::vector<uint8_t>{ 5, 0, 0, 0, 'h', 'e', 'l', 'l', 'o' }));

  SerializeMe::SpanBytesConst in(storage);
  std::string decoded;
  SerializeMe::DeserializeFromBuffer(in, decoded);
  EXPECT_EQ(decoded, text);
  EXPECT_EQ(in.size(), 0u);
}

TEST(StringEncoding, SerializeMeRejectsALengthLargerThanTheBytesThatFollow)
{
  DataTamerTest::GuardedBuffer buffer({ 0xff, 0xff, 0xff, 0xff, 'a', 'b' });
  SerializeMe::SpanBytesConst in(buffer.data(), buffer.size());
  std::string decoded;
  EXPECT_THROW(SerializeMe::DeserializeFromBuffer(in, decoded), std::runtime_error);
}
