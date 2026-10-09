#include "data_tamer/channel.hpp"
#include "data_tamer/custom_types.hpp"

#include "test_sinks.hpp"

#include <gtest/gtest.h>

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
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

struct HoldsFirst
{
  first::Pt pt;
};

template <typename AddField>
std::string_view TypeDefinition(HoldsFirst& holder, AddField& add)
{
  add("pt", &holder.pt);
  return "HoldsFirst";
}

struct Other
{
  double value = 0;
};

template <typename AddField>
std::string_view TypeDefinition(Other& other, AddField& add)
{
  add("value", &other.value);
  return "Other";
}

// A recording whose first value is a first::Pt.
struct ChannelWithFirst : DataTamerTest::Recording
{
  first::Pt first_value;

  ChannelWithFirst() { channel->registerValue("first", &first_value); }
};

// What registerCustomValue() records with a BlobSerializer.
struct Blob
{
  float a = 3;
};

class BlobSerializer : public CustomSerializer
{
public:
  BlobSerializer(std::string name, std::optional<CustomSchema> schema)
    : name_(std::move(name)), schema_(std::move(schema))
  {}
  const std::string& typeName() const override { return name_; }
  std::optional<CustomSchema> typeSchema() const override { return schema_; }
  size_t serializedSize(const void*) const override { return sizeof(float); }
  bool isFixedSize() const override { return true; }
  void serialize(const void* instance, SerializeMe::SpanBytes& buffer) const override
  {
    SerializeMe::SerializeIntoBuffer(buffer, static_cast<const Blob*>(instance)->a);
  }

private:
  std::string name_;
  std::optional<CustomSchema> schema_;
};

// Serializers that call their type "Pt", with an opaque schema and without one.
std::vector<CustomSerializer::Ptr> ptSerializers()
{
  const CustomSchema schema{ "proto", "message Pt {}" };
  return { std::make_shared<BlobSerializer>("Pt", schema),
           std::make_shared<BlobSerializer>("Pt", std::nullopt) };
}
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

// A TypeDefinition type and a registerCustomValue() serializer of one channel under one
// name: the schema would describe the serializer's values with the type's fields, or
// hold two sections of that name. The registration that comes second is refused and
// leaves the schema as it was.
TEST(CustomTypeNames, ASerializerCannotTakeTheNameOfATypeDefinitionType)
{
  for(const auto& serializer : ptSerializers())
  {
    ChannelWithFirst fixture;
    const auto before = fixture.channel->getSchema();
    Blob blob;

    EXPECT_THROW((void)fixture.channel->registerCustomValue("blob", &blob, serializer),
                 std::runtime_error);
    const auto after = fixture.channel->getSchema();
    EXPECT_EQ(ToStr(after), ToStr(before));
    EXPECT_EQ(after.hash, before.hash);
    // A serializer with a name of its own is fine.
    EXPECT_NO_THROW((void)fixture.channel->registerCustomValue(
        "blob", &blob, std::make_shared<BlobSerializer>("Blob", std::nullopt)));
  }
}

TEST(CustomTypeNames, ATypeDefinitionTypeCannotTakeTheNameOfASerializer)
{
  first::Pt pt;
  HoldsFirst holder;
  std::vector<first::Pt> many(2);
  const std::vector<std::function<void(LogChannel&)>> registrations = {
    [&](LogChannel& channel) { (void)channel.registerValue("pt", &pt); },
    [&](LogChannel& channel) { (void)channel.registerValue("holder", &holder); },
    [&](LogChannel& channel) { (void)channel.registerValue("many", &many); },
  };
  for(const auto& serializer : ptSerializers())
  {
    for(const auto& registration : registrations)
    {
      auto channel = LogChannel::create("chan");
      Blob blob;
      (void)channel->registerCustomValue("blob", &blob, serializer);
      const auto before = channel->getSchema();

      EXPECT_THROW(registration(*channel), std::runtime_error);
      const auto after = channel->getSchema();
      EXPECT_EQ(ToStr(after), ToStr(before));
      EXPECT_EQ(after.hash, before.hash);
      // A type with a name of its own is fine.
      Other other;
      EXPECT_NO_THROW((void)channel->registerValue("other", &other));
    }
  }
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
