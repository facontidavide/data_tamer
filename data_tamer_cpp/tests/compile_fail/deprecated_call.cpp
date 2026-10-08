// Built by tests/CMakeLists.txt with -Werror=deprecated-declarations: calling the
// deprecated getter must be rejected.
#include "data_tamer/channel.hpp"

uint64_t dropped(const DataTamer::LogChannel& channel,
                 const std::shared_ptr<DataTamer::SinkWorker>& sink)
{
  return channel.droppedSnapshots(sink);
}
