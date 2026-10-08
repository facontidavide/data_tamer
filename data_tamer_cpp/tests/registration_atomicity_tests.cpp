#include "data_tamer/channel.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <vector>

using namespace DataTamer;

namespace
{
struct Reading
{
  double value = 0;
};
template <typename AddField>
std::string_view TypeDefinition(Reading& reading, AddField& add)
{
  add("value", &reading.value);
  return "Reading";
}

struct Inner
{
  double a = 0;
};
template <typename AddField>
std::string_view TypeDefinition(Inner& inner, AddField& add)
{
  add("a", &inner.a);
  return "Inner";
}

struct Outer
{
  Inner first;
  std::vector<Inner> rest;
};
template <typename AddField>
std::string_view TypeDefinition(Outer& outer, AddField& add)
{
  add("first", &outer.first);
  add("rest", &outer.rest);
  return "Outer";
}

// Everything a registration can change in the schema.
void ExpectSameSchema(const Schema& after, const Schema& before)
{
  EXPECT_EQ(ToStr(after), ToStr(before));
  EXPECT_EQ(after.hash, before.hash);
  EXPECT_EQ(after.fields.size(), before.fields.size());
  EXPECT_EQ(after.custom_types.size(), before.custom_types.size());
  EXPECT_EQ(after.custom_schemas.size(), before.custom_schemas.size());
}
}  // namespace

// A registration that throws leaves no custom type behind: the type is added only
// together with a value that uses it.
TEST(RegistrationAtomicity, DuplicateNameWithANewCustomTypeChangesNothing)
{
  auto channel = LogChannel::create("atomic");
  double scalar = 0;
  channel->registerValue("value", &scalar);
  const auto before = channel->getSchema();

  Reading reading;
  EXPECT_THROW(channel->registerValue("value", &reading), std::runtime_error);
  ExpectSameSchema(channel->getSchema(), before);
  EXPECT_EQ(channel->getSchema().custom_types.count("Reading"), 0u);

  // The type is still available to a registration that succeeds.
  EXPECT_NO_THROW(channel->registerValue("reading", &reading));
  EXPECT_EQ(channel->getSchema().custom_types.count("Reading"), 1u);
}

TEST(RegistrationAtomicity, PreviousTypeMismatchWithANewCustomTypeChangesNothing)
{
  auto channel = LogChannel::create("atomic");
  double scalar = 0;
  const auto id = channel->registerValue("value", &scalar);
  channel->unregister(id);
  const auto before = channel->getSchema();

  Reading reading;
  EXPECT_THROW(channel->registerValue("value", &reading), std::runtime_error);
  ExpectSameSchema(channel->getSchema(), before);

  // The slot still takes its own type.
  const auto again = channel->registerValue("value", &scalar);
  EXPECT_TRUE(channel->isEnabled(again));
}

TEST(RegistrationAtomicity, AFailedNestedTypeLeavesNoneOfItsTypes)
{
  auto channel = LogChannel::create("atomic");
  double scalar = 0;
  channel->registerValue("value", &scalar);
  const auto before = channel->getSchema();

  Outer outer;
  EXPECT_THROW(channel->registerValue("value", &outer), std::runtime_error);
  ExpectSameSchema(channel->getSchema(), before);
  EXPECT_EQ(channel->getSchema().custom_types.count("Outer"), 0u);
  EXPECT_EQ(channel->getSchema().custom_types.count("Inner"), 0u);

  // After the failures the schema is what a channel that never failed has.
  channel->registerValue("outer", &outer);
  auto fresh = LogChannel::create("atomic");
  fresh->registerValue("value", &scalar);
  fresh->registerValue("outer", &outer);
  ExpectSameSchema(channel->getSchema(), fresh->getSchema());
  EXPECT_EQ(channel->getSchema().custom_types.size(), 2u);
}
