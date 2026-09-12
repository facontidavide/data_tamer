// Golden test for docs/wire_format.md. The fixture files under
// docs/wire_format/vectors/ are the source of truth: a change in the schema text
// or in the snapshot bytes fails here and must be a deliberate format revision.
//
// Regenerate the fixtures with DATA_TAMER_UPDATE_GOLDEN=1, then review the diff
// and update expected.json by hand.
#include "data_tamer/data_tamer.hpp"
#include "data_tamer/sinks/dummy_sink.hpp"
#include "data_tamer/sinks/mcap_sink.hpp"
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

// Sinks without a worker thread, so delivery happens exactly when the test drains
// them: no race between the worker and finishQueueAndStop(), deterministic order.
struct SyncSink : DummySink
{
  SyncSink() { stopThread(); }
};
struct SyncMcap : MCAPSink
{
  explicit SyncMcap(const std::string& path) : MCAPSink(path, false) { stopThread(); }
};

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
  auto sink = std::make_shared<SyncSink>();
  auto mcap = std::make_shared<SyncMcap>(mcap_path);
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
  ASSERT_TRUE(channel->takeSnapshot(stamp));
  sink->flush();
  const Snapshot full = sink->latestSnapshot();

  const std::string schema_text = ToStr(channel->getSchema());
  checkGolden("schema.txt", { schema_text.begin(), schema_text.end() });
  EXPECT_EQ(channel->getSchema().hash, SchemaTextHash(schema_text));
  checkGolden("snapshot_full.mask", full.active_mask);
  checkGolden("snapshot_full.payload", full.payload);

  // Disable one scalar and one custom type: their bits clear and their bytes
  // disappear from the payload; everything else keeps its relative order.
  channel->setEnabled(id_i16, false);
  channel->setEnabled(id_pose, false);
  ASSERT_TRUE(channel->takeSnapshot(stamp));
  sink->flush();
  const Snapshot masked = sink->latestSnapshot();
  checkGolden("snapshot_masked.mask", masked.active_mask);
  checkGolden("snapshot_masked.payload", masked.payload);

  // The MCAP records MCAPSink actually wrote: schema, channel and message bodies.
  mcap->finishQueueAndStop();
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
    const auto* data = reinterpret_cast<const uint8_t*>(view.message.data);
    bodies.emplace_back(data, data + view.message.dataSize);
  }
  reader.close();
  std::filesystem::remove(mcap_path);
  ASSERT_EQ(bodies.size(), 2u);
  checkGolden("snapshot_full.mcap_message", bodies[0]);
  checkGolden("snapshot_masked.mcap_message", bodies[1]);
}
