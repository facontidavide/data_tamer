// Compile-fail probe (see data_tamer_compile_fail_test in tests/CMakeLists.txt).
// As written the result is used and the file compiles. DT_IGNORE_REQUEST_DUMP drops
// it, which must then be an error.
#include "data_tamer/sinks/mcap_ring_sink.hpp"

namespace probe
{
bool request(DataTamer::MCAPRingSink& sink)
{
#ifdef DT_IGNORE_REQUEST_DUMP
  sink.requestDump();
  return true;
#else
  return sink.requestDump();
#endif
}
}  // namespace probe
