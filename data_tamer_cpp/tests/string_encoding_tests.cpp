#include "data_tamer/channel.hpp"
#include "data_tamer/contrib/SerializeMe.hpp"
#include "data_tamer/custom_types.hpp"
#include "data_tamer/sinks/dummy_sink.hpp"
#include "data_tamer_parser/data_tamer_parser.hpp"
#include "guarded_buffer.hpp"
#include "test_sinks.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <string>
#include <variant>
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

std::vector<uint8_t> payloadOf(const DataTamer::Snapshot& snapshot)
{
  return { snapshot.payload.begin(), snapshot.payload.end() };
}

// What the standalone parser makes of the snapshot, with the schema the channel wrote.
std::map<std::string, double> decode(const DataTamer::LogChannel& channel,
                                     const DataTamer::Snapshot& snapshot)
{
  const auto schema =
      DataTamerParser::BuildSchemaFromText(DataTamer::ToStr(channel.getSchema()));
  const DataTamerParser::SnapshotView view{
    snapshot.schema_hash,
    uint64_t(snapshot.timestamp.count()),
    { snapshot.active_mask.data(), snapshot.active_mask.size() },
    { snapshot.payload.data(), snapshot.payload.size() }
  };
  std::map<std::string, double> values;
  const bool complete = DataTamerParser::ParseSnapshot(
      schema, view, [&](const std::string& name, const DataTamerParser::VarNumber& v) {
        values[name] = std::visit([](const auto& x) { return double(x); }, v);
      });
  EXPECT_TRUE(complete) << "bytes are left after the last field";
  return values;
}
}  // namespace

TEST(StringEncoding, StringMemberIsSerializedAsACharVector)
{
  auto channel = DataTamer::LogChannel::create("strings");
  DataTamerTest::Attached<DataTamer::DummySink> sink;
  Named named;
  named.name = "abc";
  channel->registerValue("n", &named);
  channel->addDataSink(sink);
  ASSERT_EQ(channel->takeSnapshot(), DataTamer::SnapshotResult::ok);
  sink.drain();

  const auto snapshot = sink->latestSnapshot();
  EXPECT_EQ(payloadOf(snapshot), (std::vector<uint8_t>{ 1, 3, 0, 0, 0, 'a', 'b', 'c' }));
  EXPECT_NE(DataTamer::ToStr(channel->getSchema()).find("char[] name\n"),
            std::string::npos);
  EXPECT_EQ(decode(*channel, snapshot), (std::map<std::string, double>{
                                            { "n/tag", 1 },
                                            { "n/name[0]", 'a' },
                                            { "n/name[1]", 'b' },
                                            { "n/name[2]", 'c' },
                                        }));
}

TEST(StringEncoding, RegisteredStringIsSerializedAsACharVector)
{
  auto channel = DataTamer::LogChannel::create("strings");
  DataTamerTest::Attached<DataTamer::DummySink> sink;
  std::string text = "abc";
  channel->registerValue("s", &text);
  channel->addDataSink(sink);
  ASSERT_EQ(channel->takeSnapshot(), DataTamer::SnapshotResult::ok);
  sink.drain();

  EXPECT_EQ(payloadOf(sink->latestSnapshot()),
            (std::vector<uint8_t>{ 3, 0, 0, 0, 'a', 'b', 'c' }));
}

TEST(StringEncoding, StringLongerThan64KiBIsSerialized)
{
  auto channel = DataTamer::LogChannel::create("strings");
  DataTamerTest::Attached<DataTamer::DummySink> sink;
  Named named;
  named.name = std::string(70 * 1024, 'x');
  channel->registerValue("n", &named);
  channel->addDataSink(sink);
  channel->startLogging();

  DataTamer::SnapshotResult result = DataTamer::SnapshotResult::rejected;
  EXPECT_NO_THROW(result = channel->tryTakeSnapshot());
  EXPECT_EQ(result, DataTamer::SnapshotResult::ok);
  sink.drain();
  EXPECT_EQ(sink->latestPayloadSize(), size_t{ 1 + 4 + 70 * 1024 });
}

TEST(StringEncoding, StringThatGrowsPast64KiBIsSerialized)
{
  auto channel = DataTamer::LogChannel::create("strings");
  DataTamerTest::Attached<DataTamer::DummySink> sink;
  Named named;
  named.name = "abc";
  channel->registerValue("n", &named);
  channel->addDataSink(sink);
  channel->startLogging();

  named.name = std::string(70 * 1024, 'x');
  DataTamer::SnapshotResult result = DataTamer::SnapshotResult::rejected;
  EXPECT_NO_THROW(result = channel->takeSnapshot());
  EXPECT_EQ(result, DataTamer::SnapshotResult::ok);
  sink.drain();
  EXPECT_EQ(sink->latestPayloadSize(), size_t{ 1 + 4 + 70 * 1024 });
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
