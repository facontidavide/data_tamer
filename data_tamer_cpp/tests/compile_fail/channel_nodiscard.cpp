// Compile-fail probe (see data_tamer_compile_fail_test in tests/CMakeLists.txt).
// As written every result is used and the file compiles. Each DT_IGNORE_* macro drops
// one, which must then be an error.
#include "data_tamer/channel.hpp"

namespace probe
{
size_t useResults(DataTamer::LogChannel& channel, const DataTamer::RegistrationID& id)
{
#ifdef DT_IGNORE_TRY_SET_ENABLED
  channel.trySetEnabled(id, true);
  size_t used = 0;
#else
  size_t used = channel.trySetEnabled(id, true) ? 1 : 0;
#endif
#ifdef DT_IGNORE_CREATE
  DataTamer::LogChannel::create("probe");
#else
  used += DataTamer::LogChannel::create("probe") ? 1 : 0;
#endif
#ifdef DT_IGNORE_NUMBER_OF_SINKS
  channel.getNumberOfSinks();
  return used;
#else
  return used + channel.getNumberOfSinks();
#endif
}
}  // namespace probe
