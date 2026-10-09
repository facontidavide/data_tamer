#include "data_tamer/channel.hpp"
#include "hang_watchdog.hpp"
#include "nested_types.hpp"

#include <gtest/gtest.h>

#include <array>
#include <string_view>
#include <vector>

using namespace DataTamer;
using DataTamerTest::Inner;

namespace
{
struct Middle
{
  Inner inner;
  std::array<Inner, 2> pair;
  std::vector<Inner> list;
};

template <typename AddField>
std::string_view TypeDefinition(Middle& middle, AddField& add)
{
  add("inner", &middle.inner);
  add("pair", &middle.pair);
  add("list", &middle.list);
  return "Middle";
}

struct Outer
{
  Middle middle;
  Inner extra;
};

template <typename AddField>
std::string_view TypeDefinition(Outer& outer, AddField& add)
{
  add("middle", &outer.middle);
  add("extra", &outer.extra);
  return "Outer";
}
}  // namespace

// The registry holds its lock while it builds a serializer, and the build walks the
// fields of the type, nested types included. A walk that came back into the registry
// would lock it twice and never return, which expectFinishes() reports as a failure.
TEST(TypesRegistryLock, NestedTypesAreBuiltWithoutComingBackIntoTheRegistry)
{
  DataTamerTest::expectFinishes([] {
    TypesRegistry registry;
    Outer outer;
    EXPECT_NE(registry.getSerializer<Outer>(), nullptr);
    EXPECT_NE(registry.getSerializer<Middle>(), nullptr);
    EXPECT_NE(registry.addType<Inner>("Inner", /*skip_if_present=*/true), nullptr);
    // The channel adds the nested types one after the other.
    LogChannel::create("channel")->registerValue("outer", &outer);
  });
}
