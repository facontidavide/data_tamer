#include "data_tamer_parser/data_tamer_parser.hpp"
#include "alloc_counter.hpp"
#include "guarded_buffer.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

// Unusual and malformed input for the standalone parser. python/test_robustness.py
// checks the same cases against the Python decoder, so the two stay consistent.

using namespace DataTamerParser;

namespace
{
const std::string kHeader = "### version: 5\n### hash: 1\n### channel_name: c\n\n";
const std::string kSeparator(59, '=');

// The byte a bool object holds; reading it through memcpy is valid for any value.
uint8_t representation(const bool& value)
{
  uint8_t raw = 0;
  std::memcpy(&raw, &value, 1);
  return raw;
}

SnapshotView viewOf(const std::vector<uint8_t>& mask, const std::vector<uint8_t>& payload)
{
  return { 1, 0, { mask.data(), mask.size() }, { payload.data(), payload.size() } };
}
}  // namespace

// A byte other than 0 or 1 must not turn into a bool object that is neither.

TEST(ParserRobustness, BoolReadFromANonZeroByteIsTrue)
{
  for(const uint8_t byte : { 0, 1, 2, 0x80, 0xff })
  {
    BufferSpan buffer{ &byte, 1 };
    const bool value = Deserialize<bool>(buffer);
    EXPECT_EQ(representation(value), byte == 0 ? 0 : 1) << "byte " << int(byte);
    EXPECT_EQ(buffer.size, 0u);
  }
}

TEST(ParserRobustness, DecodedBoolFieldIsAValidBool)
{
  const auto schema = BuildSchemaFromText(kHeader + "bool flag\n");
  for(const uint8_t byte : { 0, 1, 2, 0xff })
  {
    const std::vector<uint8_t> mask = { 1 };
    const std::vector<uint8_t> payload = { byte };
    uint8_t seen = 0xEE;
    EXPECT_TRUE(ParseSnapshot(schema, viewOf(mask, payload),
                              [&](const std::string&, const VarNumber& value) {
                                seen = representation(std::get<bool>(value));
                              }));
    EXPECT_EQ(seen, byte == 0 ? 0 : 1) << "byte " << int(byte);
  }
}

// A count read from the payload, or the length of a fixed array, must not make the
// parser work more than the payload can pay for.

namespace
{
// What ParseSnapshot() makes of a schema with one active field: whether the payload fits
// the schema exactly, how many values come out, and how long the call takes.
struct Decoded
{
  bool complete = false;
  size_t values = 0;
  std::chrono::duration<double> seconds{ 0 };
};

Decoded decode(const std::string& schema_text, const std::vector<uint8_t>& payload)
{
  const auto schema = BuildSchemaFromText(schema_text);
  const std::vector<uint8_t> mask = { 1 };
  Decoded decoded;
  const auto start = std::chrono::steady_clock::now();
  decoded.complete =
      ParseSnapshot(schema, viewOf(mask, payload),
                    [&](const std::string&, const VarNumber&) { decoded.values++; });
  decoded.seconds = std::chrono::steady_clock::now() - start;
  return decoded;
}

// Decoding these takes microseconds; a loop over the elements would take seconds.
constexpr double kInstant = 0.25;
}  // namespace

TEST(ParserRobustness, HugeCountOfZeroSizeElementsCostsNothing)
{
  // E has no field, so an element takes no byte: the count is not bounded by the payload
  const auto decoded = decode(kHeader + "E[] e\n" + kSeparator + "\nMSG: E\n",
                              { 0, 0, 0, 2 });  // count 0x02000000
  EXPECT_TRUE(decoded.complete);
  EXPECT_EQ(decoded.values, 0u);
  EXPECT_LT(decoded.seconds.count(), kInstant);
}

TEST(ParserRobustness, NestedFixedArraysOfZeroSizeElementsCostNothing)
{
  const auto decoded = decode(kHeader + "L1[5000] top\n" + kSeparator + "\nMSG: E\n" +
                                  kSeparator + "\nMSG: L1\nE[5000] e\n",
                              {});
  EXPECT_TRUE(decoded.complete);
  EXPECT_EQ(decoded.values, 0u);
  EXPECT_LT(decoded.seconds.count(), kInstant);
}

TEST(ParserRobustness, CountLargerThanThePayloadCanHoldIsRejectedBeforeAnyValue)
{
  const std::string text =
      kHeader + "Pair[] pairs\n" + kSeparator + "\nMSG: Pair\nuint32 a\nuint32 b\n";
  const auto mask = std::vector<uint8_t>{ 1 };
  size_t values = 0;
  auto count_values = [&](const std::string&, const VarNumber&) { values++; };
  const auto schema = BuildSchemaFromText(text);

  // 3 pairs need 24 bytes and 20 follow the count
  std::vector<uint8_t> payload = { 3, 0, 0, 0 };
  payload.resize(4 + 20, 7);
  EXPECT_THROW((void)ParseSnapshot(schema, viewOf(mask, payload), count_values),
               std::runtime_error);
  EXPECT_EQ(values, 0u) << "values were decoded before the count was rejected";

  // 2 pairs need exactly the 16 bytes that follow
  payload = { 2, 0, 0, 0 };
  payload.resize(4 + 16, 7);
  EXPECT_TRUE(ParseSnapshot(schema, viewOf(mask, payload), count_values));
  EXPECT_EQ(values, 4u);
}

TEST(ParserRobustness, FixedArrayLargerThanThePayloadIsRejectedBeforeAnyValue)
{
  const auto schema = BuildSchemaFromText(kHeader + "uint8[8] a\n");
  const std::vector<uint8_t> mask = { 1 };
  const std::vector<uint8_t> payload(7, 1);
  size_t values = 0;
  EXPECT_THROW(
      (void)ParseSnapshot(schema, viewOf(mask, payload),
                          [&](const std::string&, const VarNumber&) { values++; }),
      std::runtime_error);
  EXPECT_EQ(values, 0u);
}

TEST(ParserRobustness, ElementsThatHoldAVectorTakeAtLeastItsCount)
{
  const std::string text =
      kHeader + "Blob[] blobs\n" + kSeparator + "\nMSG: Blob\nuint8[] bytes\n";
  const std::vector<uint8_t> mask = { 1 };
  const auto schema = BuildSchemaFromText(text);
  auto parse = [&](const std::vector<uint8_t>& payload) {
    return ParseSnapshot(schema, viewOf(mask, payload),
                         [](const std::string&, const VarNumber&) {});
  };
  EXPECT_TRUE(
      parse({ 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }));  // 3 empty blobs
  EXPECT_THROW(parse({ 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }),
               std::runtime_error);
}

TEST(ParserRobustness, CyclicTypeInAVectorIsRejected)
{
  const auto schema =
      BuildSchemaFromText(kHeader + "A[] as\n" + kSeparator + "\nMSG: A\nA a\n");
  const std::vector<uint8_t> mask = { 1 };
  const std::vector<uint8_t> one_element = { 1, 0, 0, 0 };
  EXPECT_THROW((void)ParseSnapshot(schema, viewOf(mask, one_element),
                                   [](const std::string&, const VarNumber&) {}),
               std::runtime_error);
  const std::vector<uint8_t> no_element = { 0, 0, 0, 0 };
  EXPECT_TRUE(ParseSnapshot(schema, viewOf(mask, no_element),
                            [](const std::string&, const VarNumber&) {}));
}

TEST(ParserRobustness, MinimumSizeOfNestedArraysSaturatesInsteadOfWrapping)
{
  // 65535^5 bytes at least: the exact product wraps around to 2814706817761279
  std::string text = kHeader + "A6 top\n";
  for(int level = 6; level >= 2; level--)
  {
    text += kSeparator + "\nMSG: A" + std::to_string(level) + "\nA" +
            std::to_string(level - 1) + "[65535] inner\n";
  }
  text += kSeparator + "\nMSG: A1\nuint8 v\n";
  const auto schema = BuildSchemaFromText(text);

  detail::MinSizes cache;
  EXPECT_EQ(detail::MinFieldSize(schema.fields.at(0), schema, cache, 0),
            detail::kHugeSize);
}

TEST(ParserRobustness, NamesBeyondTheSmallStringBufferAreJoinedWhole)
{
  // long names, top-level and nested, so that every name is built on the heap
  const auto schema =
      BuildSchemaFromText(kHeader + "float64 robot/leg_front_left/knee_joint/state\n" +
                          "Pose robot/base_link/estimated_pose\nPose[2] "
                          "robot/waypoints/planned_path\n" +
                          kSeparator + "\nMSG: Pose\nfloat64 x\nfloat64 y\n");
  const std::vector<uint8_t> mask = { 0b111 };
  const std::vector<uint8_t> payload(7 * sizeof(double));  // 1 value, then 2 and 4 more
  std::vector<std::string> names;
  EXPECT_TRUE(ParseSnapshot(
      schema, viewOf(mask, payload),
      [&](const std::string& name, const VarNumber&) { names.push_back(name); }));
  EXPECT_EQ(names, (std::vector<std::string>{ "robot/leg_front_left/knee_joint/state",
                                              "robot/base_link/estimated_pose/x",
                                              "robot/base_link/estimated_pose/y",
                                              "robot/waypoints/planned_path[0]/x",
                                              "robot/waypoints/planned_path[0]/y",
                                              "robot/waypoints/planned_path[1]/x",
                                              "robot/waypoints/planned_path[1]/y" }));
}

TEST(ParserRobustness, MinSizesFindsTheTypesStoredInPlaceAndBeyond)
{
  const std::array<std::string, 7> names = { "a", "b", "c", "d", "e", "f", "g" };
  detail::MinSizes sizes;
  for(size_t i = 0; i < names.size(); i++)
  {
    EXPECT_EQ(sizes.find(names[i]), nullptr) << names[i];
    sizes.insert(names[i], 10 * i);
    for(size_t j = 0; j <= i; j++)
    {
      ASSERT_NE(sizes.find(names[j]), nullptr) << names[j] << " after " << names[i];
      EXPECT_EQ(*sizes.find(names[j]), 10 * j) << names[j] << " after " << names[i];
    }
  }
  EXPECT_EQ(sizes.find("missing"), nullptr);
}

TEST(ParserRobustness, SizeOfATypeBeyondTheInPlaceMemoIsRememberedForTheNextField)
{
  // Types A to F take 4 bytes an element and G none. Two vector fields use each, so the
  // second one takes the size from the memo, wherever the memo keeps it.
  std::string fields;
  std::string types;
  for(char type = 'A'; type <= 'G'; type++)
  {
    const std::string name(1, type);
    fields += name + "[] " + name + "1\n" + name + "[] " + name + "2\n";
    types += kSeparator + "\nMSG: " + name + "\n" + (type == 'G' ? "" : "uint32 v\n");
  }
  const auto schema = BuildSchemaFromText(kHeader + fields + types);
  const std::vector<uint8_t> mask = { 0xFF, 0x3F };  // 14 fields

  auto payloadWith = [](uint32_t count_of_f2) {
    std::vector<uint8_t> payload;
    auto put = [&](uint32_t value) {
      const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
      payload.insert(payload.end(), bytes, bytes + sizeof(value));
    };
    for(char type = 'A'; type <= 'F'; type++)
    {
      put(2);  // the first field: two elements
      put(0);
      put(0);
      put(type == 'F' ? count_of_f2 : 1);  // the second: one element
      put(0);
    }
    put(0xFFFFFFFF);  // G has no field: any count holds no value
    put(0xFFFFFFFF);
    return payload;
  };

  size_t values = 0;
  auto count_values = [&](const std::string&, const VarNumber&) { values++; };
  EXPECT_TRUE(ParseSnapshot(schema, viewOf(mask, payloadWith(1)), count_values));
  EXPECT_EQ(values, 18u);

  // F2 claims 4 elements, 16 bytes, and 12 follow: the stored size makes it fail
  const auto truncated = payloadWith(4);
  EXPECT_THROW((void)ParseSnapshot(schema, viewOf(mask, truncated), count_values),
               std::runtime_error);
}

TEST(ParserRobustness, DecodeAllocatesNothingForLongTopLevelNamesAndAVectorOfACustomType)
{
  // A top-level name is passed on as it is, not copied, and the memo of the minimum size
  // of Pose lives in the decoder's own frame. The names below the top level fit a small
  // string, so nothing is left to allocate.
  const auto schema =
      BuildSchemaFromText(kHeader + "float64 robot/leg_front_left/knee_joint/position\n" +
                          "float64 robot/leg_front_left/knee_joint/velocity\nPose[2] "
                          "poses\n" +
                          kSeparator + "\nMSG: Pose\nfloat64 x\nfloat64 y\n");
  const std::vector<uint8_t> mask = { 0b111 };
  const std::vector<uint8_t> payload(6 * sizeof(double));
  size_t values = 0;
  bool complete = false;
  size_t allocations = 0;
  {
    DataTamerTest::AllocCounter::Scope scope;
    complete = ParseSnapshot(schema, viewOf(mask, payload),
                             [&](const std::string&, const VarNumber&) { values++; });
    allocations = scope.allocations();
  }
  EXPECT_TRUE(complete);
  EXPECT_EQ(values, 6u);
  EXPECT_EQ(allocations, 0u);
}

// Schema text: headers and empty input

TEST(ParserRobustness, EmptyTextIsRejected)
{
  for(const char* text : { "", "\n", "  \n\r\n   \n" })
  {
    EXPECT_THROW(BuildSchemaFromText(text), std::runtime_error) << "[" << text << "]";
  }
}

TEST(ParserRobustness, TextWithHeadersOnlyIsASchemaWithoutFields)
{
  const auto schema = BuildSchemaFromText("### version: 5\n### hash: 9\n### "
                                          "channel_name: c\n");
  EXPECT_EQ(schema.hash, 9u);
  EXPECT_EQ(schema.channel_name, "c");
  EXPECT_TRUE(schema.fields.empty());
}

TEST(ParserRobustness, HeaderValueMayFollowTheColonDirectly)
{
  const auto schema = BuildSchemaFromText("### version:5\n### hash:7\n### "
                                          "channel_name:my chan\nfloat64 x\n");
  EXPECT_EQ(schema.hash, 7u);
  EXPECT_EQ(schema.channel_name, "my chan");
  ASSERT_EQ(schema.fields.size(), 1u);
  EXPECT_EQ(schema.fields[0].field_name, "x");
  EXPECT_EQ(schema.fields[0].type, BasicType::FLOAT64);
}

TEST(ParserRobustness, LineThatOnlyLooksLikeAHeaderIsAFieldLine)
{
  // the key must match exactly and the colon must follow it directly
  for(const char* near_miss :
      { "### hash 7", "### hashes: 7", "###hash: 7", "### Hash: 7", "### hash : 7",
        "### channel_name", "### version" })
  {
    const auto schema = BuildSchemaFromText(kHeader + near_miss + "\n");
    EXPECT_EQ(schema.hash, 1u) << near_miss;
    EXPECT_EQ(schema.channel_name, "c") << near_miss;
    EXPECT_EQ(schema.fields.size(), 1u) << near_miss;
  }
}

TEST(ParserRobustness, LegacyTextWithoutAnyHeaderIsStillRead)
{
  const auto schema = BuildSchemaFromText("float64 x\nint8[3] y\n");
  ASSERT_EQ(schema.fields.size(), 2u);
  EXPECT_EQ(schema.fields[1].field_name, "y");
  EXPECT_EQ(schema.fields[1].array_size, 3u);
}

TEST(ParserRobustness, VersionAndHashMustBeUnsignedDecimals)
{
  const std::string tail = "### channel_name: c\n\nfloat64 x\n";
  for(const char* bad :
      { "", "x", "-1", "+5", "12abc", "5.0", "1e3", "0x10", "99999999999999999999999" })
  {
    EXPECT_THROW(BuildSchemaFromText("### version: 5\n### hash: " + std::string(bad) +
                                     "\n" + tail),
                 std::runtime_error)
        << "hash [" << bad << "]";
    EXPECT_THROW(BuildSchemaFromText("### version: " + std::string(bad) +
                                     "\n### hash: 1\n" + tail),
                 std::runtime_error)
        << "version [" << bad << "]";
  }
  const auto largest =
      BuildSchemaFromText("### version: 5\n### hash: 18446744073709551615\n" + tail);
  EXPECT_EQ(largest.hash, UINT64_MAX);
}

// Schema text: custom type sections

TEST(ParserRobustness, SeparatorIsALineOfAtLeast30EqualSigns)
{
  for(const size_t length : { size_t{ 30 }, size_t{ 59 }, size_t{ 100 } })
  {
    const auto schema =
        BuildSchemaFromText(kHeader + "Pose p\n" + std::string(length, '=') +
                            " \r\nMSG: Pose\n"
                            "float64 x\n");
    ASSERT_EQ(schema.custom_types.count("Pose"), 1u) << length;
    EXPECT_EQ(schema.custom_types.at("Pose").size(), 1u) << length;
  }
}

TEST(ParserRobustness, ShorterRunOfEqualSignsIsNotASeparator)
{
  const std::string text =
      kHeader + "Pose p\n" + std::string(29, '=') + "\nMSG: Pose\nfloat64 x\n";
  EXPECT_THROW(BuildSchemaFromText(text), std::runtime_error);
}

TEST(ParserRobustness, FieldNameHoldingEqualSignsIsNotASeparator)
{
  const std::string name = "a" + std::string(35, '=') + "b";
  const auto schema = BuildSchemaFromText(kHeader + "float64 " + name + "\nint8 after\n");
  ASSERT_EQ(schema.fields.size(), 2u);
  EXPECT_EQ(schema.fields[0].field_name, name);
  EXPECT_EQ(schema.fields[1].field_name, "after");
  EXPECT_TRUE(schema.custom_types.empty());
}

TEST(ParserRobustness, TypeNameLineAfterTheSeparatorIsTrimmedAndMayFollowBlankLines)
{
  for(const char* msg_line : { "  MSG: Pose", "MSG: Pose  \r", "\n \n  MSG:   Pose" })
  {
    const auto schema = BuildSchemaFromText(kHeader + "Pose p\n" + kSeparator + "\n" +
                                            msg_line + "\nfloat64 x\n");
    ASSERT_EQ(schema.custom_types.size(), 1u) << "[" << msg_line << "]";
    EXPECT_EQ(schema.custom_types.begin()->first, "Pose") << "[" << msg_line << "]";
  }
}

TEST(ParserRobustness, SeparatorNotFollowedByATypeNameIsRejected)
{
  for(const char* after : { "float64 x\n", "MSG:\n", "" })
  {
    EXPECT_THROW(BuildSchemaFromText(kHeader + "Pose p\n" + kSeparator + "\n" + after),
                 std::runtime_error)
        << "[" << after << "]";
  }
}

// Schema text: opaque custom types (an `ENCODING:` section, docs/wire_format.md)

TEST(ParserRobustness, EncodingSectionIsAnOpaqueCustomType)
{
  const std::string text = kHeader + "Foreign f\nPose p\n" + kSeparator +
                           "\nMSG: Pose\nfloat64 x\n" + kSeparator +
                           "\nMSG: Foreign\nENCODING: ros2msg\nfloat64 a\nstring b\n";
  const auto schema = BuildSchemaFromText(text);

  ASSERT_EQ(schema.custom_schemas.size(), 1u);
  EXPECT_EQ(schema.custom_schemas.at("Foreign").encoding, "ros2msg");
  EXPECT_EQ(schema.custom_schemas.at("Foreign").schema, "float64 a\nstring b");
  EXPECT_EQ(schema.custom_types.count("Foreign"), 0u);
  EXPECT_EQ(schema.custom_types.count("Pose"), 1u);
  EXPECT_EQ(ToText(schema), text);
}

TEST(ParserRobustness, OpaqueSectionOwnsTheRestOfTheText)
{
  // the foreign schema has sections of its own: none of them is ours
  const std::string foreign = "float64 a\n" + kSeparator + "\nMSG: Inner\nint32 b\n\n";
  const auto schema =
      BuildSchemaFromText(kHeader + "Foreign f\n" + kSeparator +
                          "\nMSG: Foreign\nENCODING: proto \r\n" + foreign);
  EXPECT_EQ(schema.custom_schemas.at("Foreign").encoding, "proto");
  EXPECT_EQ(schema.custom_schemas.at("Foreign").schema,
            foreign.substr(0, foreign.size() - 1));
  EXPECT_TRUE(schema.custom_types.empty());
}

TEST(ParserRobustness, OpaqueBodyIsTheRestOfTheTextWithoutOneFinalNewline)
{
  // the writer adds the newline after the body, so a body can end in a newline of its own
  const std::vector<std::pair<std::string, std::string>> cases = {
    { "ENCODING: proto", "" },
    { "ENCODING: proto\n", "" },
    { "ENCODING: proto\n\n", "" },
    { "ENCODING: proto\n\n\n", "\n" },
    { "ENCODING: proto\nbody", "body" },
    { "ENCODING: proto\nbody\n", "body" },
    { "ENCODING: proto\nbody\n\n", "body\n" },
  };
  for(const auto& [tail, body] : cases)
  {
    const auto schema = BuildSchemaFromText(kHeader + "Foreign f\n" + kSeparator +
                                            "\nMSG: Foreign\n" + tail);
    EXPECT_EQ(schema.custom_schemas.at("Foreign").encoding, "proto") << tail;
    EXPECT_EQ(schema.custom_schemas.at("Foreign").schema, body) << tail;
  }
}

TEST(ParserRobustness, EncodingLineThatDoesNotOpenASectionIsRejected)
{
  const std::string head = kHeader + "Pose p\n";
  for(const std::string& text : { kHeader + "ENCODING: proto\n", head + kSeparator +
                                                                     "\nMSG: "
                                                                     "Pose\nfloat64 "
                                                                     "x\nENCODING: "
                                                                     "proto\nbody\n" })
  {
    EXPECT_THROW(BuildSchemaFromText(text), std::runtime_error) << text;
  }
}

// Schema text: the upper-case type names of files from before version 4

namespace
{
// "type[extent] name", the way a field line reads
std::vector<std::string> describe(const FieldsVector& fields)
{
  std::vector<std::string> out;
  for(const auto& field : fields)
  {
    std::string type = field.type_name;
    if(field.is_vector)
    {
      type += "[" + (field.array_size ? std::to_string(field.array_size) : "") + "]";
    }
    out.push_back(type + " " + field.field_name);
  }
  return out;
}
}  // namespace

TEST(ParserRobustness, FieldNamedLikeALegacyTypeKeepsItsType)
{
  // with a version line a field line is "<type> <name>", whatever the name looks like
  const auto schema = BuildSchemaFromText(kHeader +
                                          "float64 BOOL\nuint32 INT8\nPose OTHER\n"
                                          "int8[2] DOUBLE\n" +
                                          kSeparator + "\nMSG: Pose\nfloat64 x\n");
  EXPECT_EQ(describe(schema.fields),
            (std::vector<std::string>{ "float64 BOOL", "uint32 INT8", "Pose OTHER",
                                       "int8[2] DOUBLE" }));
  EXPECT_EQ(schema.fields[2].type, BasicType::OTHER);
}

TEST(ParserRobustness, LegacyTextWithoutAVersionLineIsRead)
{
  // before version 4 the name came first and the type was upper case
  const auto schema = BuildSchemaFromText("speed DOUBLE\ncount INT32[3]\nflag "
                                          "BOOL\nstamp UINT64[]\n");
  EXPECT_EQ(describe(schema.fields),
            (std::vector<std::string>{ "float64 speed", "int32[3] count", "bool flag",
                                       "uint64[] stamp" }));
  EXPECT_EQ(schema.fields[1].type, BasicType::INT32);
}

// Schema text: cost

TEST(ParserRobustness, LongRunOfSpacesIsTrimmedInLinearTime)
{
  // a million spaces before and after the field: trimming that erases one space at a
  // time would take seconds
  const std::string spaces(1'000'000, ' ');
  const std::string text = kHeader + spaces + "\n" + spaces + "float64 x" + spaces + "\n";

  const auto start = std::chrono::steady_clock::now();
  const auto schema = BuildSchemaFromText(text);
  const std::chrono::duration<double> seconds = std::chrono::steady_clock::now() - start;

  ASSERT_EQ(schema.fields.size(), 1u);
  EXPECT_EQ(schema.fields[0].field_name, "x");
  EXPECT_LT(seconds.count(), kInstant);
}

// The body of an MCAP message: uint32 mask length, mask, uint32 payload length, payload

namespace
{
std::vector<uint8_t> goldenVector(const std::string& name)
{
  std::ifstream file(std::string(DATA_TAMER_WIRE_FORMAT_DIR) + "/vectors/" + name,
                     std::ios::binary);
  return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}

std::vector<uint8_t> bytesOf(const BufferSpan& span)
{
  return { span.data, span.data + span.size };
}
}  // namespace

TEST(ParserRobustness, McapMessageBodyIsSplitIntoMaskAndPayload)
{
  const std::vector<uint8_t> body = { 2, 0, 0, 0, 0xAB, 0xCD, 3, 0, 0, 0, 1, 2, 3 };
  const auto view = SplitMcapMessage({ body.data(), body.size() });
  EXPECT_EQ(bytesOf(view.active_mask), (std::vector<uint8_t>{ 0xAB, 0xCD }));
  EXPECT_EQ(bytesOf(view.payload), (std::vector<uint8_t>{ 1, 2, 3 }));
  EXPECT_EQ(view.schema_hash, 0u);
  EXPECT_EQ(view.timestamp, 0u);
}

TEST(ParserRobustness, GoldenMcapMessagesSplitIntoTheirMaskAndPayload)
{
  for(const char* stem : { "snapshot_full", "snapshot_masked" })
  {
    const auto body = goldenVector(std::string(stem) + ".mcap_message");
    ASSERT_FALSE(body.empty()) << stem;
    const auto view = SplitMcapMessage({ body.data(), body.size() });
    EXPECT_EQ(bytesOf(view.active_mask), goldenVector(std::string(stem) + ".mask"))
        << stem;
    EXPECT_EQ(bytesOf(view.payload), goldenVector(std::string(stem) + ".payload"))
        << stem;
  }
}

TEST(ParserRobustness, McapMessageBodyWithEmptyMaskAndPayloadIsValid)
{
  const std::vector<uint8_t> body = { 0, 0, 0, 0, 0, 0, 0, 0 };
  const auto view = SplitMcapMessage({ body.data(), body.size() });
  EXPECT_EQ(view.active_mask.size, 0u);
  EXPECT_EQ(view.payload.size, 0u);
}

TEST(ParserRobustness, MalformedMcapMessageBodyIsRejected)
{
  const std::vector<std::pair<const char*, std::vector<uint8_t>>> cases = {
    { "empty", {} },
    { "mask length cut", { 1, 0, 0 } },
    { "mask longer than the body", { 9, 0, 0, 0, 1, 2 } },
    { "no payload length", { 2, 0, 0, 0, 1, 2 } },
    { "payload length cut", { 1, 0, 0, 0, 7, 1, 0 } },
    { "payload longer than the body", { 1, 0, 0, 0, 7, 100, 0, 0, 0, 1, 2, 3 } },
    { "payload length 2^32-1", { 1, 0, 0, 0, 7, 255, 255, 255, 255, 1, 2, 3 } },
    { "bytes after the payload", { 1, 0, 0, 0, 7, 1, 0, 0, 0, 9, 0xEE } },
  };
  for(const auto& [name, bytes] : cases)
  {
    // the body ends at an inaccessible page: reading past what it declares would fault
    DataTamerTest::GuardedBuffer body(bytes);
    EXPECT_THROW((void)SplitMcapMessage({ body.data(), body.size() }), std::runtime_error)
        << name;
  }
}

// Default construction

TEST(ParserRobustness, DefaultConstructedViewsAndFieldsAreEmpty)
{
  // default-initialization (not value-initialization) of an object whose storage held
  // something else leaves a member without a default member initializer as it was
  alignas(SnapshotView) unsigned char storage[sizeof(SnapshotView)];
  std::memset(storage, 0xAB, sizeof(storage));
  const auto* view = new(storage) SnapshotView;
  EXPECT_EQ(view->schema_hash, 0u);
  EXPECT_EQ(view->timestamp, 0u);
  EXPECT_EQ(view->active_mask.size, 0u);
  EXPECT_EQ(view->active_mask.data, nullptr);
  EXPECT_EQ(view->payload.size, 0u);
  EXPECT_EQ(view->payload.data, nullptr);

  alignas(TypeField) unsigned char field_storage[sizeof(TypeField)];
  std::memset(field_storage, 0xAB, sizeof(field_storage));
  const auto* field = new(field_storage) TypeField;
  EXPECT_FALSE(field->is_vector);
  EXPECT_EQ(field->array_size, 0u);
  EXPECT_EQ(field->type, BasicType::OTHER);
  field->~TypeField();
}

namespace
{
// A schema with a field `f` and a vector `fs` of the type `Foreign`, which
// `type_section` leaves undescribed or opaque. With neither field active the snapshot
// decodes. With either one active (the vector holding an element) ParseSnapshot()
// throws, and the message contains `message`.
void expectCannotDecodeForeign(const std::string& type_section,
                               const std::string& message)
{
  const auto schema =
      BuildSchemaFromText(kHeader + "Foreign f\nForeign[] fs\n" + type_section);
  const std::vector<uint8_t> no_field = { 0 };
  EXPECT_TRUE(ParseSnapshot(schema, viewOf(no_field, {}),
                            [](const std::string&, const VarNumber&) {}));

  const std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> cases = {
    { { 1 }, std::vector<uint8_t>(8) },
    { { 2 }, { 1, 0, 0, 0, 0, 0, 0, 0 } },
  };
  for(const auto& [mask, payload] : cases)
  {
    try
    {
      (void)ParseSnapshot(schema, viewOf(mask, payload),
                          [](const std::string&, const VarNumber&) {});
      ADD_FAILURE() << "decoded a field of the type Foreign";
    }
    catch(const std::runtime_error& e)
    {
      EXPECT_NE(std::string(e.what()).find(message), std::string::npos) << e.what();
    }
  }
}
}  // namespace

TEST(ParserRobustness, ActiveFieldOfAnOpaqueTypeSaysItCannotBeDecoded)
{
  expectCannotDecodeForeign(kSeparator + "\nMSG: Foreign\nENCODING: proto\nmessage "
                                         "Foreign {}\n",
                            "type Foreign has an opaque encoding");
}

TEST(ParserRobustness, ActiveFieldOfAnUndefinedTypeSaysItIsUnknown)
{
  expectCannotDecodeForeign("", "unknown type Foreign");
}
