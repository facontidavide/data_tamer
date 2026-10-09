#include "data_tamer/channel.hpp"

#include "test_sinks.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

using namespace DataTamer;
using DataTamerTest::Recording;

// Class templates with two type parameters have the shape of a container, but these
// ones describe their fields: they are custom types.
namespace
{
using Bytes = std::vector<uint8_t>;

template <typename A, typename B>
struct Pair
{
  A first{};
  B second{};
};

template <typename T, typename Tag = void>
struct Tagged
{
  T value{};
};

struct HoldsPair
{
  Pair<double, double> pair;
};

template <typename AddField>
std::string_view TypeDefinition(HoldsPair& holder, AddField& add)
{
  add("pair", &holder.pair);
  return "HoldsPair";
}

// A function found by argument-dependent lookup.
template <typename T, typename Tag, typename AddField>
std::string_view TypeDefinition(Tagged<T, Tag>& tagged, AddField& add)
{
  add("value", &tagged.value);
  return "Tagged";
}
}  // namespace

// A trait, for a type the application does not own.
template <typename A, typename B>
struct DataTamer::TypeDefinitionTrait<Pair<A, B>>
{
  template <typename AddField>
  static std::string_view define(Pair<A, B>& pair, AddField& add)
  {
    add("first", &pair.first);
    add("second", &pair.second);
    return "Pair";
  }
};

// The member is a `Pair`, not a vector of its first parameter ("float64[]").
TEST(TemplateTypes, AMemberIsDescribedByItsFields)
{
  HoldsPair holder;
  holder.pair = { 1.0, 2.0 };
  Recording recording;
  recording.channel->registerValue("holder", &holder);

  const auto schema = recording.channel->getSchema();
  const auto& fields = schema.custom_types.at("HoldsPair");
  ASSERT_EQ(fields.size(), 1u);
  EXPECT_EQ(fields[0].type_name, "Pair");
  EXPECT_FALSE(fields[0].is_vector);
  EXPECT_EQ(schema.custom_types.at("Pair").size(), 2u);
  // 1.0 and 2.0 as float64, with no element count in front.
  EXPECT_EQ(recording.payload(), (Bytes{ 0, 0, 0, 0, 0, 0, 0xF0, 0x3F,  //
                                         0, 0, 0, 0, 0, 0, 0, 0x40 }));
}

TEST(TemplateTypes, ATraitOnATwoParameterTemplateRegisters)
{
  Pair<double, int32_t> pair;
  pair.first = 1.0;
  pair.second = 5;
  Recording recording;
  recording.channel->registerValue("pair", &pair);

  const auto schema = recording.channel->getSchema();
  EXPECT_EQ(schema.fields.at(0).type_name, "Pair");
  ASSERT_EQ(schema.custom_types.at("Pair").size(), 2u);
  EXPECT_EQ(schema.custom_types.at("Pair")[1].type_name, "int32");
  EXPECT_EQ(recording.payload(), (Bytes{ 0, 0, 0, 0, 0, 0, 0xF0, 0x3F, 5, 0, 0, 0 }));
}

TEST(TemplateTypes, AFunctionOnATemplateWithADefaultedParameterRegisters)
{
  Tagged<int32_t> tagged;
  tagged.value = 9;
  Recording recording;
  recording.channel->registerValue("tagged", &tagged);

  EXPECT_EQ(recording.channel->getSchema().fields.at(0).type_name, "Tagged");
  EXPECT_EQ(recording.payload(), (Bytes{ 9, 0, 0, 0 }));
}

TEST(TemplateTypes, ContainersOfTheseTypesStillWork)
{
  std::vector<Pair<double, double>> vector(2);
  std::array<Pair<double, double>, 3> array;
  Recording recording;
  recording.channel->registerValue("vector", &vector);
  recording.channel->registerValue("array", &array);

  const auto schema = recording.channel->getSchema();
  EXPECT_EQ(schema.fields.at(0).type_name, "Pair");
  EXPECT_TRUE(schema.fields.at(0).is_vector);
  EXPECT_EQ(schema.fields.at(1).array_size, 3u);
  EXPECT_EQ(recording.payload().size(), 4u + 2 * 16u + 3 * 16u);
}

// A fixed-size type can be sized without looking at the instance.
TEST(TemplateTypes, ASerializerKnowsItIsFixedSize)
{
  TypesRegistry registry;
  const auto serializer = registry.getSerializer<Pair<double, double>>();
  const Pair<double, double> pair;

  EXPECT_TRUE(serializer->isFixedSize());
  EXPECT_EQ(serializer->serializedSize(&pair), 16u);
}
