#include "data_tamer_parser/data_tamer_parser.hpp"

#include <gtest/gtest.h>

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
