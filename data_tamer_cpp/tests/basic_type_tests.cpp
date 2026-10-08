#include "data_tamer/channel.hpp"

#include "test_sinks.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

using namespace DataTamer;
using DataTamerTest::Recording;

namespace
{
using Bytes = std::vector<uint8_t>;

enum class WideSigned : long long
{
  One = 1
};

struct Counters
{
  long long count = -1;
  unsigned long long ticks = 2;
  char16_t unit = u'\x0102';
  WideSigned wide = WideSigned::One;
};

template <typename AddField>
std::string_view TypeDefinition(Counters& counters, AddField& add)
{
  add("count", &counters.count);
  add("ticks", &counters.ticks);
  add("unit", &counters.unit);
  add("wide", &counters.wide);
  return "Counters";
}

}  // namespace

// The wire type of each fixed-width C++ type is part of the wire format.
TEST(BasicTypes, FixedWidthTypesKeepTheirWireType)
{
  EXPECT_EQ(ToStr(GetBasicType<bool>()), "bool");
  EXPECT_EQ(ToStr(GetBasicType<char>()), "char");
  EXPECT_EQ(ToStr(GetBasicType<int8_t>()), "int8");
  EXPECT_EQ(ToStr(GetBasicType<uint8_t>()), "uint8");
  EXPECT_EQ(ToStr(GetBasicType<int16_t>()), "int16");
  EXPECT_EQ(ToStr(GetBasicType<uint16_t>()), "uint16");
  EXPECT_EQ(ToStr(GetBasicType<int32_t>()), "int32");
  EXPECT_EQ(ToStr(GetBasicType<uint32_t>()), "uint32");
  EXPECT_EQ(ToStr(GetBasicType<int64_t>()), "int64");
  EXPECT_EQ(ToStr(GetBasicType<uint64_t>()), "uint64");
  EXPECT_EQ(ToStr(GetBasicType<float>()), "float32");
  EXPECT_EQ(ToStr(GetBasicType<double>()), "float64");
}

template <typename T>
class IntegralWireType : public ::testing::Test
{
};
using IntegralTypes = ::testing::Types<signed char, unsigned char, short, unsigned short,
                                       int, unsigned, long, unsigned long, long long,
                                       unsigned long long, wchar_t, char16_t, char32_t>;
TYPED_TEST_SUITE(IntegralWireType, IntegralTypes);

// int64_t is `long` here, so `long long` is a different type with the same width: the
// wire type follows size and signedness, whatever the spelling.
TYPED_TEST(IntegralWireType, HasTheSizeAndSignednessOfTheCppType)
{
  const BasicType type = GetBasicType<TypeParam>();
  ASSERT_NE(ToStr(type), "other");
  EXPECT_EQ(SizeOf(type), sizeof(TypeParam));
  EXPECT_EQ(ToStr(type).rfind("uint", 0) == 0, std::is_unsigned_v<TypeParam>);
}

// What registerValue() takes as a number; containers and custom types have their own
// overloads.
TEST(BasicTypes, NumbersAreArithmeticTypesAndEnums)
{
  EXPECT_TRUE(IsNumericType<bool>());
  EXPECT_TRUE(IsNumericType<char>());
  EXPECT_TRUE(IsNumericType<double>());
  EXPECT_TRUE(IsNumericType<long long>());
  EXPECT_TRUE(IsNumericType<WideSigned>());
  EXPECT_FALSE(IsNumericType<std::string>());
  EXPECT_FALSE(IsNumericType<Counters>());
  EXPECT_FALSE(IsNumericType<std::vector<double>>());
  EXPECT_FALSE(IsNumericType<double*>());
}

TEST(BasicTypes, EnumsFollowTheirUnderlyingType)
{
  EXPECT_EQ(ToStr(GetBasicType<WideSigned>()), "int64");
  enum class Narrow : uint16_t
  {
    A
  };
  EXPECT_EQ(ToStr(GetBasicType<Narrow>()), "uint16");
}

TEST(BasicTypes, LongLongIsRecordedAsInt64)
{
  long long value = 0x0102030405060708LL;
  Recording recording;
  recording.channel->registerValue("value", &value);

  EXPECT_EQ(ToStr(recording.field().type), "int64");
  EXPECT_EQ(recording.field().type_name, "int64");
  EXPECT_EQ(recording.payload(), (Bytes{ 8, 7, 6, 5, 4, 3, 2, 1 }));
}

TEST(BasicTypes, UnsignedLongLongIsRecordedAsUint64)
{
  unsigned long long value = 0xFF00000000000002ULL;
  Recording recording;
  recording.channel->registerValue("value", &value);

  EXPECT_EQ(ToStr(recording.field().type), "uint64");
  EXPECT_EQ(recording.payload(), (Bytes{ 2, 0, 0, 0, 0, 0, 0, 0xFF }));
}

TEST(BasicTypes, CharacterTypesAreRecordedWithTheirWidth)
{
  char16_t wide16 = u'\x1234';
  char32_t wide32 = U'\x01020304';
  Recording recording;
  recording.channel->registerValue("wide16", &wide16);
  recording.channel->registerValue("wide32", &wide32);

  EXPECT_EQ(ToStr(recording.field(0).type), "uint16");
  EXPECT_EQ(ToStr(recording.field(1).type), "uint32");
  EXPECT_EQ(recording.payload(), (Bytes{ 0x34, 0x12, 4, 3, 2, 1 }));
}

TEST(BasicTypes, EnumOverLongLongIsRecordedAsInt64)
{
  WideSigned value = WideSigned::One;
  Recording recording;
  recording.channel->registerValue("value", &value);

  EXPECT_EQ(ToStr(recording.field().type), "int64");
  EXPECT_EQ(recording.payload(), (Bytes{ 1, 0, 0, 0, 0, 0, 0, 0 }));
}

TEST(BasicTypes, ContainersOfLongLongAreRecordedAsInt64)
{
  std::vector<long long> vector = { 1, 2 };
  std::array<unsigned long long, 2> array = { 3, 4 };
  Recording recording;
  recording.channel->registerValue("vector", &vector);
  recording.channel->registerValue("array", &array);

  const TypeField vector_field = recording.field(0);
  EXPECT_EQ(ToStr(vector_field.type), "int64");
  EXPECT_TRUE(vector_field.is_vector);
  EXPECT_EQ(vector_field.array_size, 0u);
  const TypeField array_field = recording.field(1);
  EXPECT_EQ(ToStr(array_field.type), "uint64");
  EXPECT_TRUE(array_field.is_vector);
  EXPECT_EQ(array_field.array_size, 2u);
  EXPECT_EQ(recording.payload(), (Bytes{ 2, 0, 0, 0,              // vector: count
                                         1, 0, 0, 0, 0, 0, 0, 0,  //
                                         2, 0, 0, 0, 0, 0, 0, 0,  //
                                         3, 0, 0, 0, 0, 0, 0, 0,  // array: no count
                                         4, 0, 0, 0, 0, 0, 0, 0 }));
}

TEST(BasicTypes, AtomicAndLoggedLongLongAreRecordedAsInt64)
{
  std::atomic<long long> atomic{ 5 };
  Recording recording;
  recording.channel->registerValue("atomic", &atomic);
  auto logged = recording.channel->createLoggedValue<long long>("logged", 6);

  EXPECT_EQ(ToStr(recording.field(0).type), "int64");
  EXPECT_EQ(ToStr(recording.field(1).type), "int64");
  EXPECT_EQ(logged->get(), 6);
  EXPECT_EQ(recording.payload(),
            (Bytes{ 5, 0, 0, 0, 0, 0, 0, 0, 6, 0, 0, 0, 0, 0, 0, 0 }));
}

// A member of a custom type goes through the same mapping as a top-level value.
TEST(BasicTypes, StructMembersOfLongLongTypesAreRecorded)
{
  Counters counters;
  Recording recording;
  recording.channel->registerValue("counters", &counters);

  const auto schema = recording.channel->getSchema();
  const auto& fields = schema.custom_types.at("Counters");
  ASSERT_EQ(fields.size(), 4u);
  EXPECT_EQ(ToStr(fields[0].type), "int64");
  EXPECT_EQ(ToStr(fields[1].type), "uint64");
  EXPECT_EQ(ToStr(fields[2].type), "uint16");
  EXPECT_EQ(ToStr(fields[3].type), "int64");
  EXPECT_EQ(recording.payload(), (Bytes{ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                         2,    0,    0,    0,    0,    0,    0,    0,  //
                                         0x02, 0x01,                                   //
                                         1,    0,    0,    0,    0,    0,    0,    0 }));
}
