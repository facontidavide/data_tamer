// Compiled as strict C++17 (see tests/CMakeLists.txt): every core public header
// must remain usable by C++17 consumers even though the library is built as C++20.
#if __cplusplus > 201703L
#error "this translation unit must be compiled as C++17"
#endif
#include "data_tamer/data_tamer.hpp"
#include "data_tamer/channel.hpp"
#include "data_tamer/custom_types.hpp"
#include "data_tamer/data_sink.hpp"
#include "data_tamer/logged_value.hpp"
#include "data_tamer/names.hpp"
#include "data_tamer/types.hpp"
#include "data_tamer/values.hpp"
#include "data_tamer/contrib/SerializeMe.hpp"
#include "data_tamer/details/locked_reference.hpp"
#include "data_tamer/details/shared_state.hpp"
#include "data_tamer/details/snapshot_pool.hpp"
#include "data_tamer/details/write_mutex.hpp"
#include "data_tamer/sinks/dummy_sink.hpp"
#include "data_tamer/sinks/mcap_sink.hpp"
#include "data_tamer/sinks/mcap_encoding.hpp"
// sinks/ros2_publisher_sink.hpp is not checked: rclcpp itself requires C++20 on
// recent ROS 2 distributions, so that header follows rclcpp's standard.
#include "data_tamer_parser/data_tamer_parser.hpp"

namespace probe_lib
{
template <int N>
struct ProbeVec
{
  double v[N] = {};
};
}  // namespace probe_lib

template <int N>
struct DataTamer::TypeDefinitionTrait<probe_lib::ProbeVec<N>>
{
  static std::string name() { return "ProbeVec" + std::to_string(N); }
  template <class AddField>
  static void define(probe_lib::ProbeVec<N>& p, AddField& add)
  {
    add("x", &p.v[0]);
  }
};

// Instantiate the templates a consumer would.
namespace
{
struct Probe
{
  double x = 0;
};
template <class AddField>
std::string_view TypeDefinition(Probe& p, AddField& add)
{
  add("x", &p.x);
  return "Probe";
}
[[maybe_unused]] void useEverything()
{
  auto channel = DataTamer::LogChannel::create("probe");
  double d = 0;
  Probe probe;
  probe_lib::ProbeVec<2> probe_vec;
  channel->registerValue("probe_vec", &probe_vec);
  std::vector<double> v;
  channel->registerValue("d", &d);
  channel->registerValue("probe", &probe);
  channel->registerValue("v", &v);
  double joined = 0;
  const std::string joined_name = DataTamer::JoinNames("ns/", std::string("joined"));
  if(DataTamer::IsCanonicalName(joined_name))
  {
    channel->registerValue(joined_name, &joined);
  }
  auto logged = channel->createLoggedValue<double>("logged");
  logged->set(1.0);
  auto tx = channel->scopedWrite();
  channel->addDataSink(DataTamer::DummySink::create());
}
}  // namespace
