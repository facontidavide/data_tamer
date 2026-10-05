#include "data_tamer/channel.hpp"
#include "data_tamer/names.hpp"
#include "data_tamer/sinks/dummy_sink.hpp"
#include "data_tamer_parser/data_tamer_parser.hpp"

#include "../examples/geometry_types.hpp"
#include "test_sinks.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <functional>
#include <string>
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
  ADD_FAILURE() << "registration did not throw";
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

// Recursive type: validation must terminate.
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
}  // namespace

// Types described only by DataTamer::TypeDefinitionTrait (no ADL overload), and
// one with both, where the trait is the definition that is registered.
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
  add("x", &obj.x);  // valid, but not the definition in use
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

TEST(Names, RejectsEmptyComponentsInEveryOverload)
{
  auto channel = LogChannel::create("controller/walk");
  double scalar = 0;
  std::atomic<int32_t> atomic{ 0 };
  std::vector<double> vect(2);
  std::array<double, 3> array{};
  Point3D point;
  std::vector<Point3D> points(2);
  auto serializer = std::make_shared<CustomSerializerT<Point3D>>("Point3D");

  const std::vector<std::pair<std::string, std::string>> bad_names = {
    { "/loco/x", "starts with '/'" }, { "loco/x/", "ends with '/'" },
    { "loco//x", "contains '//'" },   { "", "is empty" },
    { "loco x", "contains a space" },
  };
  for(const auto& [name, reason] : bad_names)
  {
    const std::vector<std::function<void()>> registrations = {
      [&] { (void)channel->registerValue(name, &scalar); },
      [&] { (void)channel->registerValue(name, &atomic); },
      [&] { (void)channel->registerValue(name, &vect); },
      [&] { (void)channel->registerValue(name, &array); },
      [&] { (void)channel->registerValue(name, &point); },
      [&] { (void)channel->registerValue(name, &points); },
      [&] { (void)channel->registerCustomValue(name, &point, serializer); },
      [&] { (void)channel->createLoggedValue<double>(name); },
    };
    for(const auto& registration : registrations)
    {
      const auto msg = RegistrationError(registration);
      EXPECT_TRUE(Contains(msg, "channel 'controller/walk'")) << msg;
      EXPECT_TRUE(Contains(msg, "'" + name + "'")) << msg;
      EXPECT_TRUE(Contains(msg, reason)) << msg;
    }
  }
  // Rejected names leave nothing behind, not even the custom type discovered
  // while registering a Point3D.
  const auto schema = channel->getSchema();
  EXPECT_TRUE(schema.fields.empty());
  EXPECT_TRUE(schema.custom_types.empty());
}

TEST(Names, RejectsInvalidCustomFieldNames)
{
  auto channel = LogChannel::create("chan");
  BadFields bad;
  const auto msg = RegistrationError([&] { (void)channel->registerValue("bad", &bad); });
  EXPECT_TRUE(Contains(msg, "channel 'chan'")) << msg;
  EXPECT_TRUE(Contains(msg, "custom type 'BadFields'")) << msg;
  EXPECT_TRUE(Contains(msg, "'y/'")) << msg;
  EXPECT_TRUE(channel->getSchema().custom_types.empty());
  EXPECT_TRUE(channel->getSchema().fields.empty());

  // The type was not half-registered: a second attempt is rejected again.
  std::vector<BadFields> bad_vect(1);
  EXPECT_THROW((void)channel->registerValue("bad_vect", &bad_vect), std::runtime_error);
  EXPECT_TRUE(channel->getSchema().custom_types.empty());
}

TEST(Names, RejectsInvalidFieldNamesOfNestedTypes)
{
  auto channel = LogChannel::create("chan");
  OuterOk outer;
  for(const std::string name : { "o1", "o2" })
  {
    const auto msg =
        RegistrationError([&] { (void)channel->registerValue(name, &outer); });
    EXPECT_TRUE(Contains(msg, "custom type 'InnerBad'")) << msg;
    EXPECT_TRUE(Contains(msg, "'/v'")) << msg;
  }
  // Neither the outer nor the inner type was half-registered.
  const auto schema = channel->getSchema();
  EXPECT_TRUE(schema.fields.empty());
  EXPECT_TRUE(schema.custom_types.empty());

  TreeNode tree;
  EXPECT_NO_THROW((void)channel->registerValue("tree", &tree));
  EXPECT_EQ(channel->getSchema().custom_types.count("TreeNode"), 1u);
}

TEST(Names, RejectsInvalidFieldNamesOfTraitTypes)
{
  auto channel = LogChannel::create("chan");
  names_third_party::TraitBad trait_only;
  auto msg = RegistrationError([&] { (void)channel->registerValue("t", &trait_only); });
  EXPECT_TRUE(Contains(msg, "custom type 'TraitBad'")) << msg;
  EXPECT_TRUE(Contains(msg, "'a//x'")) << msg;

  // The trait wins over the ADL overload, for validation as for registration.
  names_third_party::BothBadTrait both;
  msg = RegistrationError([&] { (void)channel->registerValue("b", &both); });
  EXPECT_TRUE(Contains(msg, "'x/'")) << msg;

  const auto schema = channel->getSchema();
  EXPECT_TRUE(schema.fields.empty());
  EXPECT_TRUE(schema.custom_types.empty());
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

  const auto schema = DataTamerParser::BuildSchemaFromText(ToStr(channel->getSchema()));
  const auto snapshot = sink->latestSnapshot();
  const DataTamerParser::SnapshotView view{
    snapshot.schema_hash,
    0,
    { snapshot.active_mask.data(), snapshot.active_mask.size() },
    { snapshot.payload.data(), snapshot.payload.size() }
  };
  std::vector<std::string> names;
  ASSERT_TRUE(DataTamerParser::ParseSnapshot(
      schema, view, [&](const std::string& name, const DataTamerParser::VarNumber&) {
        names.push_back(name);
      }));
  ASSERT_EQ(names.size(), 7u);
  EXPECT_EQ(names.front(), "robot/points[0]/x");
  EXPECT_EQ(names.back(), "robot/nested/a/b");
  for(const auto& name : names)
  {
    EXPECT_EQ(name.find("//"), std::string::npos) << name;
    EXPECT_NE(name.front(), '/') << name;
    EXPECT_NE(name.back(), '/') << name;
  }
}
