// Golden test for docs/wire_format.md. The fixture files under
// docs/wire_format/vectors/ are the source of truth: a change in the schema text
// or in the snapshot bytes fails here and must be a deliberate format revision.
//
// Regenerate the fixtures with DATA_TAMER_UPDATE_GOLDEN=1, then review the diff
// and update expected.json by hand.
#include "data_tamer/channel.hpp"
#include "data_tamer/data_tamer.hpp"
#include "data_tamer/sinks/dummy_sink.hpp"
#include "data_tamer/sinks/mcap_sink.hpp"
#include "data_tamer_parser/data_tamer_parser.hpp"
#include "test_sinks.hpp"
#include "../examples/geometry_types.hpp"

#include <mcap/reader.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <unistd.h>

using namespace DataTamer;
using TestTypes::Point3D;

namespace
{
struct StampedPose
{
  Point3D position;
  uint32_t stamp = 0;
};
template <class AddField>
std::string_view TypeDefinition(StampedPose& p, AddField& add)
{
  add("position", &p.position);
  add("stamp", &p.stamp);
  return "Pose";
}

const std::string kDir = DATA_TAMER_WIRE_FORMAT_DIR;

std::vector<uint8_t> readFile(const std::string& name)
{
  std::ifstream file(kDir + "/vectors/" + name, std::ios::binary);
  return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}

// Compare (or, with DATA_TAMER_UPDATE_GOLDEN set, record) `actual` against the fixture.
void checkGolden(const std::string& name, const std::vector<uint8_t>& actual)
{
  if(std::getenv("DATA_TAMER_UPDATE_GOLDEN"))
  {
    std::ofstream file(kDir + "/vectors/" + name, std::ios::binary);
    file.write(reinterpret_cast<const char*>(actual.data()),
               std::streamsize(actual.size()));
    return;
  }
  const auto expected = readFile(name);
  ASSERT_FALSE(expected.empty()) << "missing fixture " << name;
  EXPECT_EQ(actual, expected) << "wire format changed: " << name
                              << " (see docs/wire_format.md)";
}

}  // namespace

TEST(WireFormat, SchemaTextAndSnapshotsMatchGoldenVectors)
{
  const auto mcap_path =
      (std::filesystem::temp_directory_path() /
       ("data_tamer_wire_format_" + std::to_string(::getpid()) + ".mcap"))
          .string();
  auto channel = LogChannel::create("wire_test");
  // Manual delivery: snapshots reach the sinks exactly when the test drains them.
  auto sink = DataTamerTest::manual<DummySink>();
  auto mcap = DataTamerTest::manual<MCAPSink>(mcap_path, false);
  channel->addDataSink(sink);
  channel->addDataSink(mcap);

  // One field of every basic type, in the order of the BasicType enum.
  bool flag = true;
  char letter = 'A';
  int8_t i8 = -8;
  uint8_t u8 = 200;
  int16_t i16 = -1600;
  uint16_t u16 = 60000;
  int32_t i32 = -320000;
  uint32_t u32 = 4000000000u;
  int64_t i64 = -6400000000LL;
  uint64_t u64 = 0x0102030405060708ULL;
  float f32 = 1.5f;
  double f64 = -2.25;
  // Containers and custom types.
  std::vector<double> vec = { 10.0, 20.0, 30.0 };
  std::array<int32_t, 4> arr = { 1, -2, 3, -4 };
  StampedPose pose;
  pose.position = { 1.0, 2.0, 3.0 };
  pose.stamp = 42;
  std::vector<Point3D> points = { { 4.0, 5.0, 6.0 }, { 7.0, 8.0, 9.0 } };

  channel->registerValue("flag", &flag);
  channel->registerValue("letter", &letter);
  channel->registerValue("i8", &i8);
  channel->registerValue("u8", &u8);
  const auto id_i16 = channel->registerValue("i16", &i16);
  channel->registerValue("u16", &u16);
  channel->registerValue("i32", &i32);
  channel->registerValue("u32", &u32);
  channel->registerValue("i64", &i64);
  channel->registerValue("u64", &u64);
  channel->registerValue("f32", &f32);
  channel->registerValue("f64", &f64);
  channel->registerValue("vec", &vec);
  channel->registerValue("arr", &arr);
  const auto id_pose = channel->registerValue("pose", &pose);
  channel->registerValue("points", &points);

  const std::chrono::nanoseconds stamp(1234567890);
  ASSERT_EQ(channel->takeSnapshot(stamp), SnapshotResult::ok);
  sink.drain();
  const Snapshot full = sink->latestSnapshot();

  const std::string schema_text = ToStr(channel->getSchema());
  checkGolden("schema.txt", { schema_text.begin(), schema_text.end() });
  EXPECT_EQ(channel->getSchema().hash, SchemaTextHash(schema_text));
  const std::string schema_yaml = ToYaml(channel->getSchema());
  checkGolden("schema.yaml", { schema_yaml.begin(), schema_yaml.end() });
  checkGolden("snapshot_full.mask", full.active_mask);
  checkGolden("snapshot_full.payload", full.payload);

  // Disable one scalar and one custom type: their bits clear and their bytes
  // disappear from the payload; everything else keeps its relative order.
  channel->setEnabled(id_i16, false);
  channel->setEnabled(id_pose, false);
  ASSERT_EQ(channel->takeSnapshot(stamp), SnapshotResult::ok);
  sink.drain();
  const Snapshot masked = sink->latestSnapshot();
  checkGolden("snapshot_masked.mask", masked.active_mask);
  checkGolden("snapshot_masked.payload", masked.payload);

  // The MCAP records MCAPSink actually wrote: schema, channel and message bodies.
  mcap.worker->stop();
  mcap->stopRecording();
  mcap::McapReader reader;
  ASSERT_TRUE(reader.open(mcap_path).ok());
  std::vector<std::vector<uint8_t>> bodies;
  for(const auto& view : reader.readMessages())
  {
    EXPECT_EQ(view.schema->name,
              "wire_test::" + std::to_string(channel->getSchema().hash));
    EXPECT_EQ(view.schema->encoding, "data_tamer");
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(view.schema->data.data()),
                          view.schema->data.size()),
              schema_text);
    EXPECT_EQ(view.channel->topic, "wire_test");
    EXPECT_EQ(view.channel->messageEncoding, "data_tamer");
    EXPECT_EQ(view.message.logTime, mcap::Timestamp(stamp.count()));
    EXPECT_EQ(view.message.sequence, bodies.size() + 1);  // per channel, from 1
    const auto* data = reinterpret_cast<const uint8_t*>(view.message.data);
    bodies.emplace_back(data, data + view.message.dataSize);
  }
  reader.close();
  std::filesystem::remove(mcap_path);
  ASSERT_EQ(bodies.size(), 2u);
  checkGolden("snapshot_full.mcap_message", bodies[0]);
  checkGolden("snapshot_masked.mcap_message", bodies[1]);
}

// The YAML rendering (section 2.1) of names that are "/"-separated paths:
// nesting, runs that cannot be nested, and keys that need quoting.
TEST(WireFormat, YamlSchemaMatchesGoldenVectors)
{
  auto channel = LogChannel::create("nested test");
  auto sink = DataTamerTest::manual<DummySink>();
  channel->addDataSink(sink);

  double value = 0;
  uint8_t flag = 0;
  StampedPose pose;
  channel->registerValue("arm/joint_1/position", &value);
  channel->registerValue("arm/joint_1/velocity", &value);
  channel->registerValue("arm/joint_2/position", &value);
  channel->registerValue("arm/joint_2/velocity", &value);
  channel->registerValue("arm/mode", &flag);
  channel->registerValue("state", &flag);
  channel->registerValue("arm/late", &value);  // "arm" is taken: stays flat
  channel->registerValue("single/x", &value);  // a run of one: stays flat
  channel->registerValue("dup", &flag);        // a field named like the group
  channel->registerValue("dup/a", &value);
  channel->registerValue("dup/b", &value);
  channel->registerValue("on", &flag);  // YAML 1.1 boolean: quoted
  channel->registerValue("7up/x", &value);
  channel->registerValue("7up/y", &value);  // nested under a quoted key
  channel->registerValue("cart/pose", &pose);

  const Schema schema = channel->getSchema();
  const std::string text = ToStr(schema);
  const std::string yaml = ToYaml(schema);
  checkGolden("schema_nested.txt", { text.begin(), text.end() });
  checkGolden("schema_nested.yaml", { yaml.begin(), yaml.end() });

  // Both renderings parse to the same schema, and the YAML one verifies the
  // hash through the line format it renders back to.
  const auto from_text = DataTamerParser::BuildSchemaFromText(text, true);
  const auto from_yaml = DataTamerParser::BuildSchemaFromText(yaml, true);
  EXPECT_EQ(from_yaml.hash, schema.hash);
  EXPECT_EQ(from_yaml.channel_name, "nested test");
  EXPECT_EQ(from_yaml.fields, from_text.fields);
  EXPECT_EQ(from_yaml.custom_types, from_text.custom_types);
  EXPECT_EQ(DataTamerParser::ToText(from_yaml), text);
  EXPECT_LT(yaml.size(), text.size());
}
