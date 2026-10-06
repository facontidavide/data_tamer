// Compiled as strict C++17 with warnings as errors (see tests/CMakeLists.txt).
// Checks include/data_tamer/fwd.hpp:
//  - FWD_ONLY: nothing but fwd.hpp is included, as a header that only names the
//    types would do.
//  - FWD_FIRST: fwd.hpp, then every full header.
//  - FWD_LAST: every full header, then fwd.hpp.
// A class-key or template parameter mismatch between fwd.hpp and a definition is
// an error or a -Wmismatched-tags warning, which fail the build.
#if __cplusplus > 201703L
#error "this translation unit must be compiled as C++17"
#endif

#if defined(FWD_FIRST) || defined(FWD_ONLY)
#include "data_tamer/fwd.hpp"
#endif

#ifndef FWD_ONLY
#include "data_tamer/data_tamer.hpp"
#include "data_tamer/channel.hpp"
#include "data_tamer/data_sink.hpp"
#include "data_tamer/logged_value.hpp"
#include "data_tamer/types.hpp"
#endif

#ifdef FWD_LAST
#include "data_tamer/fwd.hpp"
#endif

#include <memory>

namespace fwd_probe
{
// What a header that only names the types declares.
void takesChannel(DataTamer::LogChannel& channel);
void takesConstChannel(const DataTamer::LogChannel* channel);
std::shared_ptr<DataTamer::LogChannel> makeChannel();
void takesRegistry(DataTamer::ChannelsRegistry& registry);
void takesSink(std::shared_ptr<DataTamer::DataSink> sink);
void takesWorker(const std::shared_ptr<DataTamer::SinkWorker>& worker);
void takesSchema(const DataTamer::Schema& schema);
void takesSnapshot(const DataTamer::Snapshot& snapshot);
void takesSnapshotRef(const DataTamer::SnapshotRef& snapshot);
void takesId(const DataTamer::RegistrationID& id);
void takesLogged(const std::shared_ptr<DataTamer::LoggedValue<double>>& value);

struct Holder
{
  std::shared_ptr<DataTamer::LogChannel> channel;
  DataTamer::LogChannel* raw = nullptr;
};

// Defined in this translation unit without the definition of LogChannel.
void takesChannel(DataTamer::LogChannel& channel)
{
  (void)&channel;
}
}  // namespace fwd_probe
