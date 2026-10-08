#include "data_tamer/channel.hpp"

#include <gtest/gtest.h>

#include <vector>

using namespace DataTamer;

// get() is const; a const reference must be able to take a read guard too.
TEST(LoggedValueApi, ConstReferenceReadsThroughAGuard)
{
  auto channel = LogChannel::create("chan");
  auto logged = channel->createLoggedValue<std::vector<double>>(
      "values", std::vector<double>{ 1.0, 2.0, 3.0 });
  const LoggedValue<std::vector<double>>& view = *logged;

  auto guard = view.getConstPtr();
  ASSERT_EQ(guard->size(), 3u);
  EXPECT_EQ((*guard)[1], 2.0);
  EXPECT_EQ(view.get().back(), 3.0);  // nests inside the guard
}
