// Compile-fail probe (see data_tamer_compile_fail_test in tests/CMakeLists.txt).
// As written it reads stats() and compiles. DT_CALL_DEPRECATED calls the deprecated
// droppedSnapshots(), which must then be an error.
#include "data_tamer/channel.hpp"

namespace probe
{
uint64_t dropped(const DataTamer::LogChannel& channel,
                 const std::shared_ptr<DataTamer::SinkWorker>& sink)
{
#ifdef DT_CALL_DEPRECATED
  return channel.droppedSnapshots(sink);
#else
  (void)sink;
  return channel.stats().accepted;
#endif
}
}  // namespace probe
