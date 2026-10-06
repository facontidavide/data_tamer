// The public MCAP encoding helpers (#96): a writer of stored snapshots produces
// the same records as MCAPSink, readable with the reference parser.
#include "data_tamer/channel.hpp"
#include "data_tamer/data_tamer.hpp"
#include "data_tamer/sinks/dummy_sink.hpp"
#include "data_tamer/sinks/mcap_encoding.hpp"
#include "data_tamer_parser/data_tamer_parser.hpp"
#include "alloc_counter.hpp"
#include "test_sinks.hpp"

#include <mcap/reader.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <variant>
#include <vector>

#include <unistd.h>

using namespace DataTamer;

namespace
{
std::vector<uint8_t> readVector(const std::string& name)
{
  std::ifstream file(std::string(DATA_TAMER_WIRE_FORMAT_DIR) + "/vectors/" + name,
                     std::ios::binary);
  return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}

std::string tempPath(const std::string& tag)
{
  return (std::filesystem::temp_directory_path() /
          ("data_tamer_" + tag + "_" + std::to_string(::getpid()) + ".mcap"))
      .string();
}
}  // namespace

TEST(McapEncoding, MessageBodyMatchesGoldenVectors)
{
  for(const std::string prefix : { "snapshot_full", "snapshot_masked" })
  {
    const auto mask = readVector(prefix + ".mask");
    const auto payload = readVector(prefix + ".payload");
    const auto expected = readVector(prefix + ".mcap_message");
    ASSERT_FALSE(expected.empty()) << prefix;
    std::vector<uint8_t> body;
    mcap_encoding::EncodeMessageBody(mask, payload, body);
    EXPECT_EQ(body, expected) << prefix;
    EXPECT_EQ(body.size(), mcap_encoding::MessageBodySize(mask.size(), payload.size()));
  }
}

TEST(McapEncoding, EncodingIntoReusedBufferDoesNotAllocate)
{
  const std::vector<uint8_t> mask = { 0xFF, 0x01 };
  const std::vector<uint8_t> payload(100, 0xAB);
  std::vector<uint8_t> body;
  body.reserve(mcap_encoding::MessageBodySize(mask.size(), payload.size()));
  DataTamerTest::AllocCounter::Scope scope;
  mcap_encoding::EncodeMessageBody(mask, payload, body);
  mcap_encoding::EncodeMessageBody({ mask.data(), 1 }, { payload.data(), 10 }, body);
  EXPECT_EQ(scope.allocations(), 0u);
  EXPECT_EQ(body.size(), mcap_encoding::MessageBodySize(1, 10));
}

// Snapshots stored by the application (not pool slots) are written later with
// the helpers and read back with the MCAP reader and the reference parser.
TEST(McapEncoding, StoredSnapshotsRoundTrip)
{
  auto channel = LogChannel::create("stored");
  auto sink = DataTamerTest::manual<DummySink>();
  channel->addDataSink(sink);
  int32_t a = 0;
  double b = 0;
  channel->registerValue("a", &a);
  const auto id_b = channel->registerValue("b", &b);

  std::vector<Snapshot> stored;
  for(int i = 0; i < 3; ++i)
  {
    a = 10 * i;
    b = 0.5 * i;
    channel->setEnabled(id_b, i != 1);  // the middle one has a cleared mask bit
    ASSERT_EQ(channel->takeSnapshot(std::chrono::nanoseconds(1000 + i)),
              SnapshotResult::ok);
    sink.drain();
    stored.push_back(sink->latestSnapshot());
  }
  const Schema schema = channel->getSchema();

  const auto path = tempPath("mcap_encoding");
  {
    mcap::McapWriter writer;
    ASSERT_TRUE(
        writer.open(path, mcap::McapWriterOptions(mcap_encoding::kEncoding)).ok());
    const auto channel_id = mcap_encoding::AddChannel(writer, schema);
    std::vector<uint8_t> scratch;
    uint32_t sequence = 1;
    // Snapshot overload for the first two, span overload for the last one.
    ASSERT_TRUE(
        mcap_encoding::WriteSnapshot(writer, channel_id, sequence++, stored[0], scratch)
            .ok());
    ASSERT_TRUE(
        mcap_encoding::WriteSnapshot(writer, channel_id, sequence++, stored[1], scratch)
            .ok());
    const auto& last = stored[2];
    ASSERT_TRUE(
        mcap_encoding::WriteMessage(writer, channel_id, sequence++, last.timestamp,
                                    { last.active_mask.data(), last.active_mask.size() },
                                    { last.payload.data(), last.payload.size() }, scratch)
            .ok());
    writer.close();
  }

  mcap::McapReader reader;
  ASSERT_TRUE(reader.open(path).ok());
  std::vector<std::map<std::string, double>> values;
  std::vector<uint32_t> sequences;
  std::vector<uint64_t> stamps;
  for(const auto& view : reader.readMessages())
  {
    EXPECT_EQ(view.schema->name, "stored::" + std::to_string(schema.hash));
    EXPECT_EQ(view.schema->encoding, "data_tamer");
    EXPECT_EQ(view.channel->topic, "stored");
    EXPECT_EQ(view.channel->messageEncoding, "data_tamer");
    const std::string schema_text(reinterpret_cast<const char*>(view.schema->data.data()),
                                  view.schema->data.size());
    const auto parsed_schema =
        DataTamerParser::BuildSchemaFromText(schema_text, /*check_hash=*/true);
    EXPECT_EQ(parsed_schema.hash, schema.hash);

    DataTamerParser::BufferSpan body{ reinterpret_cast<const uint8_t*>(view.message.data),
                                      view.message.dataSize };
    DataTamerParser::SnapshotView snapshot{};
    snapshot.schema_hash = parsed_schema.hash;
    snapshot.timestamp = view.message.logTime;
    const auto mask_size = DataTamerParser::Deserialize<uint32_t>(body);
    snapshot.active_mask = { body.data, mask_size };
    body.trimFront(mask_size);
    const auto payload_size = DataTamerParser::Deserialize<uint32_t>(body);
    snapshot.payload = { body.data, payload_size };
    body.trimFront(payload_size);
    EXPECT_EQ(body.size, 0u);

    auto& fields = values.emplace_back();
    ASSERT_TRUE(DataTamerParser::ParseSnapshot(
        parsed_schema, snapshot,
        [&](const std::string& name, const DataTamerParser::VarNumber& number) {
          fields[name] = std::visit([](auto v) { return double(v); }, number);
        }));
    sequences.push_back(view.message.sequence);
    stamps.push_back(view.message.logTime);
    EXPECT_EQ(view.message.publishTime, view.message.logTime);
  }
  reader.close();
  std::filesystem::remove(path);

  ASSERT_EQ(values.size(), 3u);
  EXPECT_EQ(sequences, (std::vector<uint32_t>{ 1, 2, 3 }));
  EXPECT_EQ(stamps, (std::vector<uint64_t>{ 1000, 1001, 1002 }));
  EXPECT_EQ(values[0], (std::map<std::string, double>{ { "a", 0.0 }, { "b", 0.0 } }));
  EXPECT_EQ(values[1], (std::map<std::string, double>{ { "a", 10.0 } }));
  EXPECT_EQ(values[2], (std::map<std::string, double>{ { "a", 20.0 }, { "b", 1.0 } }));
}
