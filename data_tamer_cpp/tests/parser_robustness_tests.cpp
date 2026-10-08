#include "data_tamer_parser/data_tamer_parser.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
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
// Calls ParseSnapshot() with one active field and the callback `callback`.
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

// The old parser needs seconds for these; the new one microseconds.
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
  EXPECT_THROW(ParseSnapshot(schema, viewOf(mask, payload), count_values),
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
  EXPECT_THROW(ParseSnapshot(schema, viewOf(mask, payload),
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
  EXPECT_THROW(ParseSnapshot(schema, viewOf(mask, one_element),
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
  EXPECT_EQ(detail::MinFieldSize(schema.fields.at(0), schema.custom_types, cache, 0),
            detail::kHugeSize);
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

TEST(ParserRobustness, LegacyTextWithoutAnyHeaderIsStillRead)
{
  const auto schema = BuildSchemaFromText("float64 x\nint8[3] y\n");
  ASSERT_EQ(schema.fields.size(), 2u);
  EXPECT_EQ(schema.fields[1].field_name, "y");
  EXPECT_EQ(schema.fields[1].array_size, 3u);
}
