#include "data_tamer/channel.hpp"

#include "test_sinks.hpp"

#include <gtest/gtest.h>

#include <array>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace DataTamer;

namespace
{
// Two unrelated types that call themselves "Pt".
namespace first
{
struct Pt
{
  double x = 1.5;
  double y = 2.5;
};

template <typename AddField>
std::string_view TypeDefinition(Pt& pt, AddField& add)
{
  add("x", &pt.x);
  add("y", &pt.y);
  return "Pt";
}
}  // namespace first

namespace second
{
struct Pt
{
  int32_t x = 7;
  int32_t y = 8;
  int32_t z = 9;
};

template <typename AddField>
std::string_view TypeDefinition(Pt& pt, AddField& add)
{
  add("x", &pt.x);
  add("y", &pt.y);
  add("z", &pt.z);
  return "Pt";
}
}  // namespace second

struct HoldsSecond
{
  second::Pt pt;
};

template <typename AddField>
std::string_view TypeDefinition(HoldsSecond& holder, AddField& add)
{
  add("pt", &holder.pt);
  return "HoldsSecond";
}

// A recording whose first value is a first::Pt.
struct ChannelWithFirst : DataTamerTest::Recording
{
  first::Pt first_value;

  ChannelWithFirst() { channel->registerValue("first", &first_value); }
};
}  // namespace

// The channel keeps one serializer per type name. A second C++ type under the same name
// would be serialized with the first one's, reading past the end of its object.
TEST(CustomTypeNames, ASecondCppTypeWithTheSameNameIsRejected)
{
  ChannelWithFirst fixture;
  second::Pt second_value;

  std::string error;
  try
  {
    fixture.channel->registerValue("second", &second_value);
  }
  catch(const std::runtime_error& e)
  {
    error = e.what();
  }

  EXPECT_NE(error.find("'Pt'"), std::string::npos) << "message: " << error;
  // The rejected value was not registered: only the first Pt is recorded.
  EXPECT_EQ(fixture.channel->getSchema().fields.size(), 1u);
  EXPECT_EQ(fixture.payloadSize(), 2 * sizeof(double));
}

TEST(CustomTypeNames, AMemberWithTheSameNameIsRejected)
{
  ChannelWithFirst fixture;
  HoldsSecond holder;

  EXPECT_THROW(fixture.channel->registerValue("holder", &holder), std::runtime_error);
  EXPECT_EQ(fixture.channel->getSchema().fields.size(), 1u);
}

TEST(CustomTypeNames, AContainerOfTheSameNameIsRejected)
{
  ChannelWithFirst fixture;
  std::vector<second::Pt> many(2);
  std::array<second::Pt, 2> two;

  EXPECT_THROW(fixture.channel->registerValue("many", &many), std::runtime_error);
  EXPECT_THROW(fixture.channel->registerValue("two", &two), std::runtime_error);
  EXPECT_EQ(fixture.channel->getSchema().fields.size(), 1u);
}

TEST(CustomTypeNames, TheSameCppTypeMayBeRegisteredAgain)
{
  ChannelWithFirst fixture;
  first::Pt again;
  std::vector<first::Pt> many(2);
  std::array<first::Pt, 2> two;

  EXPECT_NO_THROW(fixture.channel->registerValue("again", &again));
  EXPECT_NO_THROW(fixture.channel->registerValue("many", &many));
  EXPECT_NO_THROW(fixture.channel->registerValue("two", &two));
  // 16 (first) + 16 (again) + 4 + 2 * 16 (many) + 2 * 16 (two)
  EXPECT_EQ(fixture.payloadSize(), 100u);
}

// Each channel has its own type names.
TEST(CustomTypeNames, AnotherChannelMayUseTheNameForAnotherType)
{
  ChannelWithFirst fixture;
  auto other = LogChannel::create("other");
  second::Pt second_value;

  EXPECT_NO_THROW(other->registerValue("second", &second_value));
}

TEST(CustomTypeNames, TheRegistryRefusesAnotherTypeUnderAStoredName)
{
  TypesRegistry registry;
  ASSERT_NE(registry.getSerializer<first::Pt>(), nullptr);

  EXPECT_THROW((void)registry.getSerializer<second::Pt>(), std::runtime_error);
  EXPECT_THROW(registry.addType<second::Pt>("Pt", /*skip_if_present=*/true),
               std::runtime_error);
  // Asking for the stored type again is fine, and so is replacing it on purpose.
  EXPECT_NO_THROW(registry.getSerializer<first::Pt>());
  EXPECT_NE(registry.addType<second::Pt>("Pt", /*skip_if_present=*/false), nullptr);
  EXPECT_NO_THROW(registry.getSerializer<second::Pt>());
}
