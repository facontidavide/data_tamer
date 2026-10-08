#include "data_tamer/channel.hpp"

#include <gtest/gtest.h>

#include <utility>
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

// Real-time threads toggle values through these: none of them can throw, and the
// type says so.
TEST(LoggedValueApi, EnableAccessorsAreNoexcept)
{
  using Registration = const RegistrationID&;
  static_assert(noexcept(
      std::declval<LogChannel&>().trySetEnabled(std::declval<Registration>(), true)));
  static_assert(noexcept(
      std::declval<const LogChannel&>().isEnabled(std::declval<Registration>())));
  static_assert(noexcept(std::declval<LoggedValue<double>&>().setEnabled(true)));
  static_assert(noexcept(std::declval<const LoggedValue<double>&>().isEnabled()));
  using Vector = LoggedValue<std::vector<double>>;
  static_assert(noexcept(std::declval<Vector&>().setEnabled(true)));
  static_assert(noexcept(std::declval<const Vector&>().isEnabled()));
}
