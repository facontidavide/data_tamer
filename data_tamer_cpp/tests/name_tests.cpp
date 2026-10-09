#include "data_tamer/channel.hpp"
#include "data_tamer/data_tamer.hpp"
#include "data_tamer/names.hpp"
#include "data_tamer/sinks/dummy_sink.hpp"

#include "../examples/geometry_types.hpp"
#include "decode_utils.hpp"
#include "test_sinks.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

using namespace DataTamer;
using namespace TestTypes;

namespace
{
// Runs `register_fn`, expects std::runtime_error and returns its message.
std::string RegistrationError(const std::function<void()>& register_fn)
{
  try
  {
    register_fn();
  }
  catch(const std::runtime_error& err)
  {
    return err.what();
  }
  ADD_FAILURE() << "no std::runtime_error thrown";
  return {};
}

bool Contains(const std::string& text, const std::string& part)
{
  return text.find(part) != std::string::npos;
}

struct BadFields
{
  double x = 0;
  double y = 0;
};
template <typename AddField>
std::string_view TypeDefinition(BadFields& obj, AddField& add)
{
  add("x", &obj.x);
  add("y/", &obj.y);
  return "BadFields";
}

struct NestedFields
{
  double x = 0;
};
template <typename AddField>
std::string_view TypeDefinition(NestedFields& obj, AddField& add)
{
  add("a/b", &obj.x);  // a non-empty nested path is fine
  return "NestedFields";
}

struct InnerBad
{
  double v = 0;
};
template <typename AddField>
std::string_view TypeDefinition(InnerBad& obj, AddField& add)
{
  add("/v", &obj.v);
  return "InnerBad";
}

struct OuterOk
{
  double a = 0;
  std::vector<InnerBad> inner;
};
template <typename AddField>
std::string_view TypeDefinition(OuterOk& obj, AddField& add)
{
  add("a", &obj.a);
  add("inner", &obj.inner);
  return "OuterOk";
}

// Recursive type.
struct TreeNode
{
  double value = 0;
  std::vector<TreeNode> children;
};
template <typename AddField>
std::string_view TypeDefinition(TreeNode& obj, AddField& add)
{
  add("value", &obj.value);
  add("children", &obj.children);
  return "TreeNode";
}

// One registration per overload, for the name tests below.
struct NameRegistrations
{
  double scalar = 0;
  std::atomic<int32_t> atomic{ 0 };
  std::vector<double> vect = std::vector<double>(2);
  std::array<double, 3> array{};
  Point3D point;
  std::vector<Point3D> points = std::vector<Point3D>(2);
  std::shared_ptr<CustomSerializerT<Point3D>> serializer =
      std::make_shared<CustomSerializerT<Point3D>>("Point3D");

  std::vector<std::function<void(LogChannel&, const std::string&)>> all()
  {
    return {
      [&](LogChannel& c, const std::string& n) { (void)c.registerValue(n, &scalar); },
      [&](LogChannel& c, const std::string& n) { (void)c.registerValue(n, &atomic); },
      [&](LogChannel& c, const std::string& n) { (void)c.registerValue(n, &vect); },
      [&](LogChannel& c, const std::string& n) { (void)c.registerValue(n, &array); },
      [&](LogChannel& c, const std::string& n) { (void)c.registerValue(n, &point); },
      [&](LogChannel& c, const std::string& n) { (void)c.registerValue(n, &points); },
      [&](LogChannel& c, const std::string& n) {
        (void)c.registerCustomValue(n, &point, serializer);
      },
      [&](LogChannel& c, const std::string& n) { (void)c.createLoggedValue<double>(n); },
    };
  }
};
}  // namespace

// Types described only by DataTamer::TypeDefinitionTrait (no ADL overload), and
// one with both, where the trait is the definition that is registered. Their
// field names are not canonical; registration accepts them.
namespace names_third_party
{
struct TraitBad
{
  double x = 0;
};
struct BothBadTrait
{
  double x = 0;
};
template <typename AddField>
std::string_view TypeDefinition(BothBadTrait& obj, AddField& add)
{
  add("x", &obj.x);  // not the definition in use
  return "BothBadTrait";
}
}  // namespace names_third_party

template <>
struct DataTamer::TypeDefinitionTrait<names_third_party::TraitBad>
{
  template <typename AddField>
  static std::string_view define(names_third_party::TraitBad& obj, AddField& add)
  {
    add("a//x", &obj.x);
    return "TraitBad";
  }
};

template <>
struct DataTamer::TypeDefinitionTrait<names_third_party::BothBadTrait>
{
  template <typename AddField>
  static std::string_view define(names_third_party::BothBadTrait& obj, AddField& add)
  {
    add("x/", &obj.x);
    return "BothBadTrait";
  }
};

TEST(Names, JoinNamesCollapsesSlashes)
{
  EXPECT_EQ(JoinNames("loco", "torso", "x"), "loco/torso/x");
  EXPECT_EQ(JoinNames("/loco/", "/torso/", "x"), "loco/torso/x");
  EXPECT_EQ(JoinNames("loco//torso/", "", "x//"), "loco/torso/x");
  EXPECT_EQ(JoinNames("/"), "");
  EXPECT_EQ(JoinNames(), "");
  EXPECT_EQ(JoinNames("x"), "x");
  const std::string ns = "controller/walk/";
  const std::string_view leaf = "LF";
  EXPECT_EQ(JoinNames(ns, leaf, std::string("q")), "controller/walk/LF/q");
}

TEST(Names, IsCanonicalName)
{
  EXPECT_TRUE(IsCanonicalName("x"));
  EXPECT_TRUE(IsCanonicalName("loco/LF/x"));
  EXPECT_TRUE(IsCanonicalName("points[0]/x"));
  EXPECT_TRUE(IsCanonicalName(JoinNames("/loco/", "LF/", "x")));
  EXPECT_FALSE(IsCanonicalName(""));
  EXPECT_FALSE(IsCanonicalName("/"));
  EXPECT_FALSE(IsCanonicalName("/loco/x"));
  EXPECT_FALSE(IsCanonicalName("loco/x/"));
  EXPECT_FALSE(IsCanonicalName("loco//x"));
  EXPECT_FALSE(IsCanonicalName("loco x"));
  EXPECT_FALSE(IsCanonicalName("loco\tx"));
  EXPECT_FALSE(IsCanonicalName("loco\nx"));
  EXPECT_FALSE(IsCanonicalName("loco\x01x"));
  EXPECT_FALSE(IsCanonicalName("loco\x7f"));
  EXPECT_TRUE(IsCanonicalName("temp\xc3\xa9rature/\xc3\xa4"));  // UTF-8 is a name
  EXPECT_FALSE(IsCanonicalName(JoinNames("/", "")));
}

TEST(Names, AcceptsHierarchicalNames)
{
  auto channel = LogChannel::create("controller/walk");
  double a = 0;
  double b = 0;
  std::vector<Point3D> points(2);
  EXPECT_NO_THROW(channel->registerValue("loco/LF/x", &a));
  EXPECT_NO_THROW(channel->registerValue(JoinNames("/loco/", "LF/", "y"), &b));
  EXPECT_NO_THROW(channel->registerValue("loco/points", &points));
  NestedFields nested;
  EXPECT_NO_THROW(channel->registerValue("nested", &nested));
  EXPECT_EQ(channel->getSchema().fields.at(1).field_name, "loco/LF/y");
}

// Names with empty '/'-separated components are accepted unchanged
// (IsCanonicalName() is opt-in).
TEST(Names, AcceptsNonCanonicalNamesInEveryOverload)
{
  NameRegistrations values;
  for(const std::string name : { "/loco/x", "loco/x/", "loco//x" })
  {
    for(const auto& registration : values.all())
    {
      auto channel = LogChannel::create("chan");
      EXPECT_NO_THROW(registration(*channel, name)) << "'" << name << "'";
      const auto schema = channel->getSchema();
      ASSERT_EQ(schema.fields.size(), 1u);
      EXPECT_EQ(schema.fields.front().field_name, name);
    }
  }
}

// The schema is a text with one field per line, so a name must be non-empty and free
// of whitespace and control characters; the error names the channel, the value and what
// is wrong with it.
TEST(Names, RejectsEmptyAndWhitespaceOrControlNamesInEveryOverload)
{
  struct BadNames
  {
    std::string problem;  // what the message says is wrong with the names
    std::vector<std::string> names;
  };
  const std::string space = "it contains a space";
  // Octal escapes: \001 is 0x01, \037 is 0x1f, \177 is 0x7f.
  const std::vector<BadNames> bad_names = {
    { "it is empty", { "" } },
    { space, { " a", "a ", "loco x" } },
    { "it contains a whitespace or control character",
      { "\t", "a\tb", "a\nb", "a\rb", "a\vb", "a\fb", "a\001b", "a\037b", "a\177b",
        std::string("a\0b", 3) } },
  };
  NameRegistrations values;
  for(const auto& group : bad_names)
  {
    for(const auto& name : group.names)
    {
      for(const auto& registration : values.all())
      {
        auto channel = LogChannel::create("controller/walk");
        const auto msg = RegistrationError([&] { registration(*channel, name); });
        EXPECT_TRUE(Contains(msg, "channel 'controller/walk'")) << msg;
        EXPECT_TRUE(Contains(msg, "invalid value name")) << msg;
        EXPECT_TRUE(Contains(msg, group.problem)) << msg;
        if(group.problem == space)
        {
          EXPECT_TRUE(Contains(msg, "'" + name + "'")) << msg;  // shown as it is
        }
        // The message stays on one line and still shows which name it was.
        for(const char c : msg)
        {
          EXPECT_FALSE(c != ' ' && static_cast<unsigned char>(c) <= 0x20) << msg;
        }
        if(!name.empty() && std::isalpha(static_cast<unsigned char>(name.front())) != 0)
        {
          EXPECT_TRUE(Contains(msg, "'" + name.substr(0, 1))) << msg;
        }
        if(!name.empty() && std::isalpha(static_cast<unsigned char>(name.back())) != 0)
        {
          EXPECT_TRUE(Contains(msg, name.substr(name.size() - 1) + "'")) << msg;
        }
        // A rejected name leaves nothing behind, custom types included.
        const auto schema = channel->getSchema();
        EXPECT_TRUE(schema.fields.empty());
        EXPECT_TRUE(schema.custom_types.empty());
      }
    }
  }
}

// Bytes above 0x7f are UTF-8, not control characters.
TEST(Names, AcceptsUtf8Names)
{
  NameRegistrations values;
  for(const auto& registration : values.all())
  {
    auto channel = LogChannel::create("chan");
    const std::string name = "temp\xc3\xa9rature/\xc3\xa4\xe2\x82\xac";
    EXPECT_NO_THROW(registration(*channel, name));
    ASSERT_EQ(channel->getSchema().fields.size(), 1u);
    EXPECT_EQ(channel->getSchema().fields.front().field_name, name);
  }
}

// Field names of a TypeDefinition / TypeDefinitionTrait (nested types included) with
// empty '/'-separated components are accepted, as value names are.
TEST(Names, AcceptsNonCanonicalCustomFieldNames)
{
  auto channel = LogChannel::create("chan");
  BadFields bad;
  OuterOk outer;
  names_third_party::TraitBad trait_only;
  names_third_party::BothBadTrait both;
  TreeNode tree;
  EXPECT_NO_THROW((void)channel->registerValue("bad", &bad));
  EXPECT_NO_THROW((void)channel->registerValue("outer", &outer));
  EXPECT_NO_THROW((void)channel->registerValue("t", &trait_only));
  EXPECT_NO_THROW((void)channel->registerValue("b", &both));
  EXPECT_NO_THROW((void)channel->registerValue("tree", &tree));

  const auto schema = channel->getSchema();
  EXPECT_EQ(schema.custom_types.at("BadFields").at(1).field_name, "y/");
  EXPECT_EQ(schema.custom_types.at("InnerBad").at(0).field_name, "/v");
  EXPECT_EQ(schema.custom_types.at("TraitBad").at(0).field_name, "a//x");
  // The trait wins over the ADL overload.
  EXPECT_EQ(schema.custom_types.at("BothBadTrait").at(0).field_name, "x/");
  EXPECT_EQ(schema.custom_types.count("TreeNode"), 1u);
}

namespace
{
// A name the schema text cannot hold as a type or field name, and how an error
// message shows it.
struct BadName
{
  const char* name;
  const char* shown;
};
// \177 is DEL (0x7f).
constexpr std::array<BadName, 5> kBadNames{ {
    { "a b", "a b" },
    { "a\nb", "a\\x0ab" },
    { "a\tb", "a\\x09b" },
    { "a\177b", "a\\x7fb" },
    { "", "" },
} };

// Custom types whose type name, nested type name or field name is kBadNames[I].name.
template <size_t I>
struct BadTypeName
{
  double x = 0;
};
template <size_t I, typename AddField>
const char* TypeDefinition(BadTypeName<I>& obj, AddField& add)
{
  add("x", &obj.x);
  return kBadNames[I].name;
}

template <size_t I>
struct HoldsBadTypeName
{
  BadTypeName<I> inner;
};
template <size_t I, typename AddField>
const char* TypeDefinition(HoldsBadTypeName<I>& obj, AddField& add)
{
  add("inner", &obj.inner);
  return "HoldsBadTypeName";
}

template <size_t I>
struct BadFieldName
{
  double x = 0;
};
template <size_t I, typename AddField>
const char* TypeDefinition(BadFieldName<I>& obj, AddField& add)
{
  add(kBadNames[I].name, &obj.x);
  return "BadFieldName";
}

// Calls `check` with std::integral_constant<size_t, I> for each index I of kBadNames.
template <typename Check, size_t... I>
void ForEachBadName(Check check, std::index_sequence<I...> /*indices*/)
{
  (check(std::integral_constant<size_t, I>{}), ...);
}
template <typename Check>
void ForEachBadName(Check check)
{
  ForEachBadName(check, std::make_index_sequence<kBadNames.size()>{});
}

// Runs `registration` on a channel holding one scalar and returns the message of the
// std::runtime_error it must throw. The schema text and hash must stay as they were.
std::string RejectedRegistration(const std::function<void(LogChannel&)>& registration)
{
  double scalar = 0;
  auto channel = LogChannel::create("chan");
  channel->registerValue("scalar", &scalar);
  const auto before = channel->getSchema();
  const auto msg = RegistrationError([&] { registration(*channel); });
  const auto after = channel->getSchema();
  EXPECT_EQ(ToStr(after), ToStr(before));
  EXPECT_EQ(after.hash, before.hash);
  return msg;
}
}  // namespace

// Custom type names and their field names are written into the schema text like value
// names, one "<type> <name>" line per field, so the same rules apply to them, nested
// types included.
TEST(Names, RejectsCustomTypeAndFieldNamesTheSchemaCannotHold)
{
  ForEachBadName([](auto index) {
    constexpr size_t kIndex = decltype(index)::value;
    const std::string shown = "'" + std::string(kBadNames[kIndex].shown) + "'";
    SCOPED_TRACE("bad name " + shown);
    BadTypeName<kIndex> bad_type;
    HoldsBadTypeName<kIndex> holds_bad_type;
    BadFieldName<kIndex> bad_field;

    auto msg = RejectedRegistration(
        [&](LogChannel& channel) { (void)channel.registerValue("value", &bad_type); });
    EXPECT_TRUE(Contains(msg, shown)) << msg;

    msg = RejectedRegistration([&](LogChannel& channel) {
      (void)channel.registerValue("value", &holds_bad_type);
    });
    EXPECT_TRUE(Contains(msg, shown)) << msg;

    msg = RejectedRegistration(
        [&](LogChannel& channel) { (void)channel.registerValue("value", &bad_field); });
    EXPECT_TRUE(Contains(msg, shown)) << msg;
    EXPECT_TRUE(Contains(msg, "'BadFieldName'")) << msg;
  });
}

// The type name of a registerCustomValue() serializer is the type of its field line.
TEST(Names, RejectsSerializerTypeNamesTheSchemaCannotHold)
{
  for(const auto& bad : kBadNames)
  {
    const std::string shown = "'" + std::string(bad.shown) + "'";
    SCOPED_TRACE("bad name " + shown);
    Point3D point;
    auto serializer = std::make_shared<CustomSerializerT<Point3D>>(bad.name);
    const auto msg = RejectedRegistration([&](LogChannel& channel) {
      (void)channel.registerCustomValue("point", &point, serializer);
    });
    EXPECT_TRUE(Contains(msg, shown)) << msg;
  }
}

// The channel name fills the schema header line "### channel_name: <name>": spaces are
// fine there, but a control character, a line break above all, breaks the text.
TEST(Names, RejectsChannelNamesTheSchemaCannotHold)
{
  const std::vector<BadName> bad_names = {
    { "", "" },
    { "a\nb", "a\\x0ab" },
    { "a\rb", "a\\x0db" },
    { "a\tb", "a\\x09b" },
    { "a\001b", "a\\x01b" },
    { "a\177b", "a\\x7fb" },
  };
  ChannelsRegistry registry;
  for(const auto& bad : bad_names)
  {
    const std::string shown = "'" + std::string(bad.shown) + "'";
    SCOPED_TRACE("bad name " + shown);
    auto msg = RegistrationError([&] { (void)LogChannel::create(bad.name); });
    EXPECT_TRUE(Contains(msg, shown)) << msg;
    msg = RegistrationError([&] { (void)registry.getChannel(bad.name); });
    EXPECT_TRUE(Contains(msg, shown)) << msg;
  }
}

namespace
{
struct Utf8Names
{
  double x = 0;
};
template <typename AddField>
std::string_view TypeDefinition(Utf8Names& obj, AddField& add)
{
  add("\xc3\xa4", &obj.x);
  return "Temp\xc3\xa9rature";
}
}  // namespace

// Bytes above 0x7f are UTF-8 in type, field and channel names too, and a channel name
// can hold spaces.
TEST(Names, AcceptsUtf8InEveryNameAndSpacesInChannelNames)
{
  const std::string channel_name = "robot arm/\xc3\xa4";
  auto channel = LogChannel::create(channel_name);
  EXPECT_EQ(channel->channelName(), channel_name);
  ChannelsRegistry registry;
  EXPECT_EQ(registry.getChannel(channel_name)->channelName(), channel_name);

  Utf8Names utf8;
  Point3D point;
  auto serializer = std::make_shared<CustomSerializerT<Point3D>>("Punkt\xc3\xa4");
  EXPECT_NO_THROW((void)channel->registerValue("utf8", &utf8));
  EXPECT_NO_THROW((void)channel->registerCustomValue("point", &point, serializer));
  const auto schema = channel->getSchema();
  EXPECT_EQ(schema.custom_types.at("Temp\xc3\xa9rature").at(0).field_name, "\xc3\xa4");
  EXPECT_EQ(schema.fields.at(1).type_name, "Punkt\xc3\xa4");
}

TEST(Names, ErrorsNameChannelAndValue)
{
  auto channel = LogChannel::create("controller/walk");
  DataTamerTest::Attached<DummySink> sink;
  channel->addDataSink(sink);
  double x = 0;
  double other = 0;
  int32_t wrong_type = 0;
  auto id = channel->registerValue("loco/LF/x", &x);

  auto msg =
      RegistrationError([&] { (void)channel->registerValue("loco/LF/x", &other); });
  EXPECT_EQ(msg, "channel 'controller/walk': value 'loco/LF/x' registered twice "
                 "(unregister() it first)");

  channel->unregister(id);
  msg =
      RegistrationError([&] { (void)channel->registerValue("loco/LF/x", &wrong_type); });
  EXPECT_TRUE(Contains(msg, "channel 'controller/walk'")) << msg;
  EXPECT_TRUE(Contains(msg, "'loco/LF/x'")) << msg;
  EXPECT_TRUE(Contains(msg, "different type")) << msg;

  ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
  msg = RegistrationError([&] { (void)channel->registerValue("loco/LF/y", &other); });
  EXPECT_TRUE(Contains(msg, "channel 'controller/walk'")) << msg;
  EXPECT_TRUE(Contains(msg, "'loco/LF/y'")) << msg;
  EXPECT_TRUE(Contains(msg, "recording started")) << msg;

  Point3D point;
  msg = RegistrationError([&] { (void)channel->registerValue("point", &point); });
  EXPECT_TRUE(Contains(msg, "channel 'controller/walk'")) << msg;
  EXPECT_TRUE(Contains(msg, "'Point3D'")) << msg;
}

TEST(Names, FlattenedNamesHaveNoEmptyComponents)
{
  auto channel = LogChannel::create("chan");
  DataTamerTest::Attached<DummySink> sink;
  channel->addDataSink(sink);
  std::vector<Point3D> points(2);
  NestedFields nested;
  channel->registerValue(JoinNames("robot/", "/points"), &points);
  channel->registerValue("robot/nested", &nested);
  ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
  sink.drain();

  const auto values = DataTamerTest::decode(*channel, sink->latestSnapshot());
  ASSERT_EQ(values.size(), 7u);
  EXPECT_EQ(values.count("robot/points[0]/x"), 1u);
  EXPECT_EQ(values.count("robot/nested/a/b"), 1u);
  for(const auto& entry : values)
  {
    const std::string& name = entry.first;
    EXPECT_EQ(name.find("//"), std::string::npos) << name;
    EXPECT_NE(name.front(), '/') << name;
    EXPECT_NE(name.back(), '/') << name;
  }
}
