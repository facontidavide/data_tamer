#include "data_tamer/channel.hpp"
#include "nested_types.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <future>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

using namespace DataTamer;
using namespace std::chrono_literals;
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
// would lock it twice. Watchdog: such a call would never return.
TEST(TypesRegistryLock, NestedTypesAreBuiltWithoutComingBackIntoTheRegistry)
{
  auto registry = std::make_shared<TypesRegistry>();
  auto outer = std::make_shared<Outer>();
  auto finished = std::make_shared<std::promise<void>>();
  auto future = finished->get_future();
  std::thread worker([registry, outer, finished] {
    EXPECT_NE(registry->getSerializer<Outer>(), nullptr);
    EXPECT_NE(registry->getSerializer<Middle>(), nullptr);
    EXPECT_NE(registry->addType<Inner>("Inner", /*skip_if_present=*/true), nullptr);
    // The channel adds the nested types one after the other.
    LogChannel::create("channel")->registerValue("outer", outer.get());
    finished->set_value();
  });

  if(future.wait_for(10s) != std::future_status::ready)
  {
    worker.detach();  // it is stuck on the registry lock
    FAIL() << "building the serializers did not return";
  }
  worker.join();
}
