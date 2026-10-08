// Built by tests/CMakeLists.txt with -Werror=unused-result: the statement selected by
// PROBE_* discards a [[nodiscard]] result, so this file must be rejected.
#include "data_tamer/channel.hpp"

void discard([[maybe_unused]] DataTamer::LogChannel& channel,
             [[maybe_unused]] const DataTamer::RegistrationID& id)
{
#if defined(PROBE_TRY_SET_ENABLED)
  channel.trySetEnabled(id, true);
#elif defined(PROBE_CREATE)
  DataTamer::LogChannel::create("probe");
#elif defined(PROBE_NUMBER_OF_SINKS)
  channel.getNumberOfSinks();
#else
#error "select the statement to discard with a PROBE_* macro"
#endif
}
