// Includes no data_tamer header but logged_value.hpp: the members called below must be
// defined there, or this program does not link. It is built by a test, never run.
#include "data_tamer/logged_value.hpp"

#include <cstddef>
#include <vector>

double useScalar(DataTamer::LoggedValue<double>& value)
{
  value.set(1.5);
  value.setEnabled(false);
  return value.isEnabled() ? 0.0 : value.get();
}

std::size_t useVector(DataTamer::LoggedValue<std::vector<double>>& value)
{
  value.set({ 1.0, 2.0 });
  value.getMutablePtr()->push_back(3.0);
  value.setEnabled(false);
  return value.getConstPtr()->size() + value.get().size() + value.isEnabled();
}

int main()
{
  return 0;
}
