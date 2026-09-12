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
#include "data_tamer/types.hpp"
#include "data_tamer/values.hpp"
#include "data_tamer/contrib/SerializeMe.hpp"
#include "data_tamer/details/locked_reference.hpp"
#include "data_tamer/details/shared_state.hpp"
#include "data_tamer/details/snapshot_pool.hpp"
#include "data_tamer/details/write_mutex.hpp"
#include "data_tamer/sinks/dummy_sink.hpp"
#include "data_tamer/sinks/mcap_sink.hpp"
// sinks/ros2_publisher_sink.hpp is not checked: rclcpp itself requires C++20 on
// recent ROS 2 distributions, so that header follows rclcpp's standard.
#include "data_tamer_parser/data_tamer_parser.hpp"

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
  std::vector<double> v;
  channel->registerValue("d", &d);
  channel->registerValue("probe", &probe);
  channel->registerValue("v", &v);
  auto logged = channel->createLoggedValue<double>("logged");
  logged->set(1.0);
  auto tx = channel->scopedWrite();
  channel->addDataSink(std::make_shared<DataTamer::DummySink>());
}
}  // namespace
