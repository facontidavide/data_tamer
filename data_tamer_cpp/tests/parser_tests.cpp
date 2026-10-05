#include "data_tamer_parser/data_tamer_parser.hpp"
#include "data_tamer/data_tamer.hpp"
#include "data_tamer/sinks/dummy_sink.hpp"
#include "../examples/geometry_types.hpp"
#include "test_sinks.hpp"

#include <gtest/gtest.h>
#include <thread>
#include <variant>
#include <cstring>
#include <locale>
#include <string>

using namespace DataTamerParser;
using namespace TestTypes;

TEST(DataTamerParser, ReadSchema)
{
  const char* text = R"(
int8 v1
float64 v2
float32[5] array
int32[] vect
bool is_true
char[256] blob
uint16  my/short
  )";

  const auto schema = BuildSchemaFromText(text);

  ASSERT_EQ(schema.fields.size(), 7);

  TypeField field0 = { "v1", BasicType::INT8, "int8", false, 0 };
  ASSERT_EQ(schema.fields[0], field0);

  TypeField field1 = { "v2", BasicType::FLOAT64, "float64", false, 0 };
  ASSERT_EQ(schema.fields[1], field1);

  TypeField field2 = { "array", BasicType::FLOAT32, "float32", true, 5 };
  ASSERT_EQ(schema.fields[2], field2);

  TypeField field3 = { "vect", BasicType::INT32, "int32", true, 0 };
  ASSERT_EQ(schema.fields[3], field3);

  TypeField field4 = { "is_true", BasicType::BOOL, "bool", false, 0 };
  ASSERT_EQ(schema.fields[4], field4);

  TypeField field5 = { "blob", BasicType::CHAR, "char", true, 256 };
  ASSERT_EQ(schema.fields[5], field5);

  TypeField field6 = { "my/short", BasicType::UINT16, "uint16", false, 0 };
  ASSERT_EQ(schema.fields[6], field6);
}

TEST(DataTamerParser, SchemaHash)
{
  auto channel = DataTamer::LogChannel::create("chan");

  // logs in channelA
  std::vector<double> v1(10, 0);
  std::array<float, 4> v2 = { 1, 2, 3, 4 };
  int32_t v3 = 5;
  uint16_t v4 = 6;
  double v5 = 10;
  uint16_t v6 = 11;
  std::vector<uint8_t> v7(4, 12);
  std::array<uint32_t, 3> v8 = { 13, 14, 15 };

  channel->registerValue("vector_10", &v1);
  channel->registerValue("array_4", &v2);
  channel->registerValue("val_int32", &v3);
  channel->registerValue("val_int16", &v4);
  channel->registerValue("real_value", &v5);
  channel->registerValue("short_int", &v6);
  channel->registerValue("vector_4", &v7);
  channel->registerValue("array_3", &v8);

  const auto schema_in = channel->getSchema();
  std::ostringstream ss;
  ss << schema_in;

  const auto& schema_out = BuildSchemaFromText(ss.str());

  ASSERT_EQ(schema_out.fields[0].field_name, "vector_10");
  ASSERT_EQ(schema_out.fields[1].field_name, "array_4");
  ASSERT_EQ(schema_out.fields[2].field_name, "val_int32");
  ASSERT_EQ(schema_out.fields[3].field_name, "val_int16");
  ASSERT_EQ(schema_out.fields[4].field_name, "real_value");
  ASSERT_EQ(schema_out.fields[5].field_name, "short_int");
  ASSERT_EQ(schema_out.fields[6].field_name, "vector_4");
  ASSERT_EQ(schema_out.fields[7].field_name, "array_3");
}

TEST(DataTamerParser, CustomTypes)
{
  auto channel = DataTamer::LogChannel::create("chan");

  Pose pose;
  channel->registerValue("pose", &pose);

  const auto& schema_in = channel->getSchema();
  const std::string schema_txt = ToStr(schema_in);
  const auto& schema_out = DataTamerParser::BuildSchemaFromText(schema_txt);

  std::cout << schema_txt << std::endl;

  ASSERT_EQ(schema_in.hash, schema_out.hash);
  ASSERT_EQ(schema_in.channel_name, schema_out.channel_name);
  ASSERT_EQ(schema_in.fields.size(), schema_out.fields.size());
  ASSERT_EQ(schema_in.custom_types.size(), schema_out.custom_types.size());

  for(const auto& [type_name, custom_in] : schema_in.custom_types)
  {
    const auto& custom_out = schema_out.custom_types.at(type_name);
    ASSERT_EQ(custom_in.size(), custom_out.size());

    for(size_t i = 0; i < custom_in.size(); i++)
    {
      const auto& field_in = custom_in[i];
      const auto& field_out = custom_out[i];

      ASSERT_EQ(field_in.field_name, field_out.field_name);
      ASSERT_EQ(static_cast<int>(field_in.type), static_cast<int>(field_out.type));
      ASSERT_EQ(field_in.is_vector, field_out.is_vector);
      ASSERT_EQ(field_in.array_size, field_out.array_size);
    }
  }
}

SnapshotView ConvertSnapshot(const DataTamer::Snapshot& snapshot)
{
  return { snapshot.schema_hash,
           uint64_t(snapshot.timestamp.count()),
           { snapshot.active_mask.data(), snapshot.active_mask.size() },
           { snapshot.payload.data(), snapshot.payload.size() } };
}

TEST(DataTamerParser, PlainParsing)
{
  auto channel = DataTamer::LogChannel::create("channel");
  DataTamerTest::Attached<DataTamer::DummySink> dummy_sink;
  channel->addDataSink(dummy_sink);

  int32_t v1 = 5;
  uint16_t v2 = 6;
  double v3 = 7;
  uint16_t v4 = 8;

  channel->registerValue("v1", &v1);
  channel->registerValue("v2", &v2);
  channel->registerValue("v3", &v3);
  channel->registerValue("v4", &v4);

  ASSERT_EQ(channel->takeSnapshot(), DataTamer::SnapshotResult::ok);
  dummy_sink.drain();

  const auto& schema_in = channel->getSchema();
  const auto& schema_out = DataTamerParser::BuildSchemaFromText(ToStr(schema_in));
  const auto snapshot = dummy_sink->latestSnapshot();
  const auto snapshot_view = ConvertSnapshot(snapshot);

  std::map<std::string, double> parsed_values;
  auto callback = [&](const std::string& field_name,
                      const DataTamerParser::VarNumber& number) {
    parsed_values[field_name] =
        std::visit([](const auto& var) { return double(var); }, number);
  };

  DataTamerParser::ParseSnapshot(schema_out, snapshot_view, callback);

  for(const auto& [name, value] : parsed_values)
  {
    std::cout << name << ": " << value << std::endl;
  }

  ASSERT_EQ(parsed_values.at("v1"), 5);
  ASSERT_EQ(parsed_values.at("v2"), 6);
  ASSERT_EQ(parsed_values.at("v3"), 7);
  ASSERT_EQ(parsed_values.at("v4"), 8);
}

TEST(DataTamerParser, CustomParsing)
{
  DataTamer::ChannelsRegistry registry;
  auto channel = registry.getChannel("channel");
  DataTamerTest::Attached<DataTamer::DummySink> dummy_sink;
  channel->addDataSink(dummy_sink);

  Pose pose;
  pose.pos = { 1, 2, 3 };
  pose.rot = { 4, 5, 6, 7 };
  channel->registerValue("pose", &pose);

  ASSERT_EQ(channel->takeSnapshot(), DataTamer::SnapshotResult::ok);
  dummy_sink.drain();

  const auto& schema_in = channel->getSchema();
  const auto& schema_out = DataTamerParser::BuildSchemaFromText(ToStr(schema_in));
  const auto snapshot = dummy_sink->latestSnapshot();
  const auto snapshot_view = ConvertSnapshot(snapshot);

  std::map<std::string, double> parsed_values;
  auto callback = [&](const std::string& field_name,
                      const DataTamerParser::VarNumber& number) {
    const double value = std::visit([](const auto& var) { return double(var); }, number);
    parsed_values[field_name] = value;
  };

  DataTamerParser::ParseSnapshot(schema_out, snapshot_view, callback);

  for(const auto& [name, value] : parsed_values)
  {
    std::cout << name << ": " << value << std::endl;
  }

  ASSERT_EQ(parsed_values.at("pose/position/x"), 1);
  ASSERT_EQ(parsed_values.at("pose/position/y"), 2);
  ASSERT_EQ(parsed_values.at("pose/position/z"), 3);

  ASSERT_EQ(parsed_values.at("pose/rotation/w"), 4);
  ASSERT_EQ(parsed_values.at("pose/rotation/x"), 5);
  ASSERT_EQ(parsed_values.at("pose/rotation/y"), 6);
  ASSERT_EQ(parsed_values.at("pose/rotation/z"), 7);
}

TEST(DataTamerParser, VectorParsing)
{
  DataTamer::ChannelsRegistry registry;
  auto channel = registry.getChannel("channel");
  DataTamerTest::Attached<DataTamer::DummySink> dummy_sink;
  channel->addDataSink(dummy_sink);

  std::vector<double> valsA = { 10, 11, 12 };
  std::array<int, 2> valsB = { 13, 14 };

  std::array<Point3D, 3> points;
  points[0] = { 1, 2, 3 };
  points[1] = { 4, 5, 6 };
  points[2] = { 7, 8, 9 };
  std::vector<Quaternion> quats(2);
  quats[0] = { 20, 21, 22, 23 };
  quats[1] = { 30, 31, 32, 33 };

  channel->registerValue("valsA", &valsA);
  channel->registerValue("valsB", &valsB);
  channel->registerValue("points", &points);
  channel->registerValue("quats", &quats);

  ASSERT_EQ(channel->takeSnapshot(), DataTamer::SnapshotResult::ok);
  dummy_sink.drain();

  const auto& schema_in = channel->getSchema();
  const auto& schema_out = DataTamerParser::BuildSchemaFromText(ToStr(schema_in));
  const auto snapshot = dummy_sink->latestSnapshot();
  const auto snapshot_view = ConvertSnapshot(snapshot);

  std::map<std::string, double> parsed_values;
  auto callback = [&](const std::string& field_name,
                      const DataTamerParser::VarNumber& number) {
    const double value = std::visit([](const auto& var) { return double(var); }, number);
    parsed_values[field_name] = value;
  };

  DataTamerParser::ParseSnapshot(schema_out, snapshot_view, callback);

  for(const auto& [name, value] : parsed_values)
  {
    std::cout << name << ": " << value << std::endl;
  }

  ASSERT_EQ(parsed_values.at("valsA[0]"), 10);
  ASSERT_EQ(parsed_values.at("valsA[1]"), 11);
  ASSERT_EQ(parsed_values.at("valsA[2]"), 12);

  ASSERT_EQ(parsed_values.at("valsB[0]"), 13);
  ASSERT_EQ(parsed_values.at("valsB[1]"), 14);

  ASSERT_EQ(parsed_values.at("points[0]/x"), 1);
  ASSERT_EQ(parsed_values.at("points[0]/y"), 2);
  ASSERT_EQ(parsed_values.at("points[0]/z"), 3);

  ASSERT_EQ(parsed_values.at("points[1]/x"), 4);
  ASSERT_EQ(parsed_values.at("points[1]/y"), 5);
  ASSERT_EQ(parsed_values.at("points[1]/z"), 6);

  ASSERT_EQ(parsed_values.at("points[2]/x"), 7);
  ASSERT_EQ(parsed_values.at("points[2]/y"), 8);
  ASSERT_EQ(parsed_values.at("points[2]/z"), 9);

  ASSERT_EQ(parsed_values.at("quats[0]/w"), 20);
  ASSERT_EQ(parsed_values.at("quats[0]/x"), 21);
  ASSERT_EQ(parsed_values.at("quats[0]/y"), 22);
  ASSERT_EQ(parsed_values.at("quats[0]/z"), 23);

  ASSERT_EQ(parsed_values.at("quats[1]/w"), 30);
  ASSERT_EQ(parsed_values.at("quats[1]/x"), 31);
  ASSERT_EQ(parsed_values.at("quats[1]/y"), 32);
  ASSERT_EQ(parsed_values.at("quats[1]/z"), 33);
}

// Version 4 texts (std::hash recipe) must still parse and verify exactly as before;
// version 5 texts verify with the platform-independent recipe.
TEST(DataTamerParser, ReadsAndVerifiesBothSchemaVersions)
{
  auto channel = DataTamer::LogChannel::create("chan");
  Pose pose;
  int32_t count = 0;
  channel->registerValue("pose", &pose);
  channel->registerValue("count", &count);
  const std::string v5_text = ToStr(channel->getSchema());

  // version 5: declared hash equals the defined recomputation
  const auto v5 = BuildSchemaFromText(v5_text, /*check_hash=*/true);
  ASSERT_EQ(v5.hash, SchemaTextHash(v5_text));
  ASSERT_EQ(v5.hash, channel->getSchema().hash);
  auto broken = v5_text;
  broken.replace(broken.find("### hash: ") + 10, 1, "9");
  EXPECT_THROW(BuildSchemaFromText(broken, true), std::runtime_error);

  // version 4: the same fields, hashed with the legacy recipe
  uint64_t legacy = std::hash<std::string>()(v5.channel_name);
  for(const auto& field : v5.fields)
  {
    legacy = AddFieldToHash(field, legacy);
  }
  std::string v4_text = v5_text;
  v4_text.replace(v4_text.find("### version: 5"), 14, "### version: 4");
  const auto hash_pos = v4_text.find("### hash: ") + 10;
  v4_text.replace(hash_pos, v4_text.find('\n', hash_pos) - hash_pos,
                  std::to_string(legacy));
  const auto v4 = BuildSchemaFromText(v4_text, /*check_hash=*/true);
  ASSERT_EQ(v4.hash, legacy);
  ASSERT_EQ(v4.fields.size(), v5.fields.size());
  ASSERT_EQ(v4.custom_types.size(), v5.custom_types.size());

  EXPECT_THROW(BuildSchemaFromText("### version: 3\n"), std::runtime_error);
}

// Corrupt or hostile input must fail with an exception, never read out of bounds.
TEST(DataTamerParser, RejectsMalformedInput)
{
  auto channel = DataTamer::LogChannel::create("chan");
  Pose pose;
  std::vector<double> samples = { 1.0, 2.0 };
  channel->registerValue("pose", &pose);
  channel->registerValue("samples", &samples);
  DataTamerTest::Attached<DataTamer::DummySink> sink;
  channel->addDataSink(sink);
  ASSERT_EQ(channel->takeSnapshot(), DataTamer::SnapshotResult::ok);
  sink.drain();
  const auto snapshot = sink->latestSnapshot();
  const auto schema = BuildSchemaFromText(ToStr(channel->getSchema()));
  auto count_values = [](const std::string&, const VarNumber&) {};

  // intact
  SnapshotView view{ snapshot.schema_hash,
                     0,
                     { snapshot.active_mask.data(), snapshot.active_mask.size() },
                     { snapshot.payload.data(), snapshot.payload.size() } };
  ASSERT_TRUE(ParseSnapshot(schema, view, count_values));

  // truncated payload: the last double is cut in half; the checked read reports it
  SnapshotView truncated = view;
  truncated.payload.size -= 4;
  try
  {
    ParseSnapshot(schema, truncated, count_values);
    FAIL() << "truncated payload accepted";
  }
  catch(const std::runtime_error& e)
  {
    EXPECT_NE(std::string(e.what()).find("truncated"), std::string::npos) << e.what();
  }
  static_assert(sizeof(SnapshotView::schema_hash) == 8, "schema hash must be 64-bit "
                                                        "everywhere");
  static_assert(sizeof(DataTamer::Snapshot::schema_hash) == 8, "schema hash must be "
                                                               "64-bit everywhere");
  EXPECT_FALSE(schema.fields[0] !=
               schema.fields[0]);  // operator!= must be defined (link check)
  EXPECT_TRUE(schema.fields[0] != schema.fields[1]);

  // mask too short for the schema
  SnapshotView short_mask = view;
  short_mask.active_mask.size = 0;
  EXPECT_THROW(ParseSnapshot(schema, short_mask, count_values), std::runtime_error);

  // dynamic vector whose count exceeds the payload
  auto huge = snapshot.payload;
  const size_t count_offset = huge.size() - 2 * sizeof(double) - sizeof(uint32_t);
  const uint32_t bogus = 0xFFFFFFFFu;
  std::memcpy(huge.data() + count_offset, &bogus, sizeof(bogus));
  SnapshotView huge_view = view;
  huge_view.payload = { huge.data(), huge.size() };
  EXPECT_THROW(ParseSnapshot(schema, huge_view, count_values), std::runtime_error);

  // a custom type name that merely starts with a primitive name is a custom type
  const auto tricky = BuildSchemaFromText("### version: 5\n### hash: 1\n### "
                                          "channel_name: "
                                          "c\n\n"
                                          "float64Pose p\n"
                                          "=============================================="
                                          "="
                                          "============\n"
                                          "MSG: float64Pose\nfloat64 x\n");
  ASSERT_EQ(tricky.fields.size(), 1u);
  EXPECT_EQ(tricky.fields[0].type, BasicType::OTHER);
  EXPECT_EQ(tricky.fields[0].type_name, "float64Pose");

  // a self-referencing custom type cannot be traversed forever
  const auto cyclic = BuildSchemaFromText("### version: 5\n### hash: 1\n### "
                                          "channel_name: "
                                          "c\n\n"
                                          "Node root\n"
                                          "=============================================="
                                          "="
                                          "============\n"
                                          "MSG: Node\nNode next\n");
  const uint8_t one_bit = 1;
  const uint8_t no_payload = 0;
  SnapshotView cyclic_view{ 1, 0, { &one_bit, 1 }, { &no_payload, 0 } };
  EXPECT_THROW(ParseSnapshot(cyclic, cyclic_view, count_values), std::runtime_error);

  // malformed array extents
  EXPECT_THROW(BuildSchemaFromText("### version: 5\n### hash: 1\n### channel_name: "
                                   "c\n\nint32[0] a\n"),
               std::runtime_error);
  EXPECT_THROW(BuildSchemaFromText("### version: 5\n### hash: 1\n### channel_name: "
                                   "c\n\nint32[70000] a\n"),
               std::runtime_error);
  EXPECT_THROW(BuildSchemaFromText("### version: 5\n### hash: 1\n### channel_name: "
                                   "c\n\nint32[3 a\n"),
               std::runtime_error);
}

namespace
{
// Same field names as the data_tamer_msgs types, without depending on ROS.
struct FakeSchemaMsg
{
  uint64_t hash;
  std::string channel_name;
  std::string schema_text;
};
struct FakeSnapshotMsg
{
  uint64_t timestamp_nsec;
  uint64_t schema_hash;
  std::vector<uint8_t> active_mask;
  std::vector<uint8_t> payload;
};
struct FakeBatchMsg
{
  std::vector<FakeSchemaMsg> schemas;
  std::vector<FakeSnapshotMsg> snapshots;
};
}  // namespace

TEST(DataTamerParser, SnapshotBatchHelpers)
{
  auto channel = DataTamer::LogChannel::create("batch_channel");
  DataTamerTest::Attached<DataTamer::DummySink> dummy_sink;
  channel->addDataSink(dummy_sink);

  int32_t v1 = 0;
  double v2 = 0;
  channel->registerValue("v1", &v1);
  channel->registerValue("v2", &v2);
  const auto schema_in = channel->getSchema();

  FakeBatchMsg batch;
  batch.schemas.push_back({ schema_in.hash, schema_in.channel_name, ToStr(schema_in) });
  for(int i = 0; i < 3; i++)
  {
    v1 = i;
    v2 = 0.5 * i;
    ASSERT_EQ(channel->takeSnapshot(), DataTamer::SnapshotResult::ok);
    dummy_sink.drain();
    const auto snapshot = dummy_sink->latestSnapshot();
    batch.snapshots.push_back({ uint64_t(snapshot.timestamp.count()),
                                snapshot.schema_hash, snapshot.active_mask,
                                snapshot.payload });
  }
  // a snapshot whose schema is unknown is skipped
  batch.snapshots.push_back({ 0, schema_in.hash + 1, {}, {} });

  DataTamerParser::SchemaRegistry registry;
  std::vector<std::map<std::string, double>> decoded;
  const size_t visited = DataTamerParser::ForEachSnapshotInBatch(
      registry, batch,
      [&](const DataTamerParser::Schema& schema,
          const DataTamerParser::SnapshotView& view) {
        EXPECT_EQ(schema.channel_name, "batch_channel");
        auto& values = decoded.emplace_back();
        EXPECT_TRUE(DataTamerParser::ParseSnapshot(
            schema, view,
            [&](const std::string& name, const DataTamerParser::VarNumber& n) {
              values[name] = std::visit([](const auto& var) { return double(var); }, n);
            }));
      });

  ASSERT_EQ(visited, 3u);
  ASSERT_EQ(registry.size(), 1u);
  ASSERT_NE(registry.find(schema_in.hash), nullptr);
  ASSERT_EQ(decoded.size(), 3u);
  for(int i = 0; i < 3; i++)
  {
    EXPECT_EQ(decoded[size_t(i)].at("v1"), i);
    EXPECT_EQ(decoded[size_t(i)].at("v2"), 0.5 * i);
  }

  // without embedded schemas, a registry filled earlier (e.g. from the
  // `schemas` topic) still decodes the batch
  batch.schemas.clear();
  EXPECT_EQ(DataTamerParser::ForEachSnapshotInBatch(registry, batch,
                                                    [](const auto&, const auto&) {}),
            3u);
  DataTamerParser::SchemaRegistry empty_registry;
  EXPECT_EQ(DataTamerParser::ForEachSnapshotInBatch(empty_registry, batch,
                                                    [](const auto&, const auto&) {}),
            0u);
}

TEST(DataTamerParser, YamlSchemaRoundTrip)
{
  DataTamer::Schema schema;
  schema.channel_name = "yaml";
  schema.fields = { { "a/b/c", DataTamer::BasicType::FLOAT64, "", false, 0 },
                    { "a/b/d", DataTamer::BasicType::INT32, "", true, 0 },
                    { "a/e", DataTamer::BasicType::OTHER, "Custom Type", true, 3 },
                    { "x", DataTamer::BasicType::UINT8, "", false, 0 } };
  schema.custom_types["Custom Type"] = { { "v", DataTamer::BasicType::BOOL, "", false,
                                           0 } };
  schema.custom_schemas["Blob"] = { "proto\"buf", "line 1\n\tline \"2\"\x01\nlast" };
  schema.hash = DataTamer::ComputeSchemaHash(schema);

  const auto yaml = DataTamer::ToYaml(schema);
  const auto parsed = BuildSchemaFromText(yaml, true);
  EXPECT_EQ(parsed.hash, schema.hash);
  ASSERT_EQ(parsed.fields.size(), 4u);
  EXPECT_EQ(parsed.fields[0].field_name, "a/b/c");
  EXPECT_EQ(parsed.fields[1].field_name, "a/b/d");
  EXPECT_TRUE(parsed.fields[1].is_vector);
  EXPECT_EQ(parsed.fields[2].type_name, "Custom Type");
  EXPECT_EQ(parsed.fields[2].array_size, 3u);
  EXPECT_EQ(parsed.fields[3].type, BasicType::UINT8);
  ASSERT_EQ(parsed.custom_schemas.count("Blob"), 1u);
  EXPECT_EQ(parsed.custom_schemas.at("Blob").encoding, "proto\"buf");
  EXPECT_EQ(parsed.custom_schemas.at("Blob").schema, "line 1\n\tline \"2\"\x01\nlast");
  EXPECT_EQ(ToText(parsed), DataTamer::ToStr(schema));
  EXPECT_EQ(DataTamer::RenderSchema(schema, DataTamer::SchemaFormat::Yaml), yaml);
  EXPECT_EQ(DataTamer::RenderSchema(schema, DataTamer::SchemaFormat::Text),
            DataTamer::ToStr(schema));

  // empty mappings
  DataTamer::Schema empty;
  empty.channel_name = "empty";
  empty.hash = DataTamer::ComputeSchemaHash(empty);
  EXPECT_TRUE(BuildSchemaFromText(DataTamer::ToYaml(empty), true).fields.empty());
}

TEST(DataTamerParser, YamlSchemaRejectsMalformedInput)
{
  const std::string head = "version: 6\nhash: 1\nchannel_name: c\n";
  EXPECT_THROW(BuildSchemaFromText("version: 7\nhash: 1\nchannel_name: c\nfields: {}\n"),
               std::runtime_error);
  EXPECT_THROW(BuildSchemaFromText(head), std::runtime_error);  // no fields
  EXPECT_THROW(BuildSchemaFromText(head + "fields:\n  a: float64\n  a: int8\n"),
               std::runtime_error);  // duplicate key
  EXPECT_THROW(BuildSchemaFromText(head + "fields:\n  a: float64\n   b: int8\n"),
               std::runtime_error);  // indentation
  EXPECT_THROW(BuildSchemaFromText(head + "fields:\n  a: [float64]\n"),
               std::runtime_error);  // flow sequence
  EXPECT_THROW(BuildSchemaFromText(head + "fields:\n  \"a: float64\n"),
               std::runtime_error);  // unterminated string
  EXPECT_THROW(BuildSchemaFromText(head + "fields:\n  a: int32[0]\n"),
               std::runtime_error);
  EXPECT_THROW(BuildSchemaFromText(head + "fields:\n  a: float64\n", true),
               std::runtime_error);  // wrong hash
  // numbers too large for uint64 / an array extent: runtime_error, not out_of_range
  EXPECT_THROW(BuildSchemaFromText(head + "fields:\n  a: "
                                          "int32[99999999999999999999999]\n"),
               std::runtime_error);
  EXPECT_THROW(BuildSchemaFromText("version: 6\nhash: "
                                   "99999999999999999999\nchannel_name: "
                                   "c\nfields: {}\n"),
               std::runtime_error);
  EXPECT_THROW(BuildSchemaFromText(head + "fields:\n  \"\\uD800\": int8\n"),
               std::runtime_error);  // surrogate
  EXPECT_THROW(BuildSchemaFromText("  version: 6\n  hash: 1\n  channel_name: c\n  "
                                   "fields: {}\n"),
               std::runtime_error);  // top level must start at column 0
  std::string deep = head + "fields:\n";
  for(int i = 0; i < 100; i++)
  {
    deep += std::string(size_t(2 * (i + 1)), ' ') + "k:\n";
  }
  EXPECT_THROW(BuildSchemaFromText(deep), std::runtime_error);
}

TEST(DataTamerParser, YamlSchemaIsLocaleIndependentAndEscapesLineBreaks)
{
  // a global locale with digit grouping must not leak into the hash line
  struct Grouping : std::numpunct<char>
  {
    char do_thousands_sep() const override { return ','; }
    std::string do_grouping() const override { return "\3"; }
  };
  const std::locale previous =
      std::locale::global(std::locale(std::locale(), new Grouping));

  DataTamer::Schema schema;
  schema.channel_name = "line\u2028sep";
  // NEL, LS and PS are line breaks for YAML 1.1 loaders: they must be escaped
  schema.fields = { { "a\u0085b", DataTamer::BasicType::INT8, "", false, 0 },
                    { "c\u2029d", DataTamer::BasicType::INT8, "", false, 0 },
                    { "caf\u00e9", DataTamer::BasicType::INT8, "", false, 0 } };
  schema.hash = DataTamer::ComputeSchemaHash(schema);
  const auto yaml = DataTamer::ToYaml(schema);
  std::locale::global(previous);

  EXPECT_NE(yaml.find("hash: " + std::to_string(schema.hash) + "\n"), std::string::npos)
      << yaml;
  EXPECT_EQ(yaml.find("\u2028"), std::string::npos);
  EXPECT_EQ(yaml.find("\u2029"), std::string::npos);
  EXPECT_EQ(yaml.find("\u0085"), std::string::npos);
  EXPECT_NE(yaml.find("\"line\\u2028sep\""), std::string::npos) << yaml;
  EXPECT_NE(yaml.find("\"a\\x85b\""), std::string::npos) << yaml;
  EXPECT_NE(yaml.find("\"caf\u00e9\""), std::string::npos) << yaml;  // other UTF-8 as is

  const auto parsed = BuildSchemaFromText(yaml, true);
  EXPECT_EQ(parsed.channel_name, "line\u2028sep");
  ASSERT_EQ(parsed.fields.size(), 3u);
  EXPECT_EQ(parsed.fields[0].field_name, "a\u0085b");
  EXPECT_EQ(parsed.fields[1].field_name, "c\u2029d");
}

TEST(DataTamerParser, SchemaRegistryRejectsHashMismatch)
{
  DataTamer::Schema schema;
  schema.channel_name = "c";
  schema.fields = { { "a", DataTamer::BasicType::INT8, "", false, 0 } };
  schema.hash = DataTamer::ComputeSchemaHash(schema);
  DataTamerParser::SchemaRegistry registry;
  EXPECT_THROW(registry.add(schema.hash + 1, DataTamer::ToStr(schema)),
               std::runtime_error);
  EXPECT_EQ(registry.size(), 0u);
  EXPECT_EQ(registry.add(schema.hash, DataTamer::ToYaml(schema)).fields.size(), 1u);
}
