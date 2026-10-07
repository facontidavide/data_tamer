// What a consumer compiles into its own binaries, as one shared object that
// tools/abi_check.sh builds against the baseline headers and against the current ones
// and compares with libabigail, like libdata_tamer.so itself.
//
// libdata_tamer.so alone cannot show every change that matters. The library never
// defines CustomSerializer or LoggedValue<T>, and consumers compile the registerValue
// templates, LoggedValue::set and the scoped write guards. Deriving from DataSink and
// CustomSerializer and instantiating the templates here puts their layouts and
// vtables into the debug info. Each exported function takes or returns the types
// whose layout is in question, so that abidiff reaches them.
//
// The probe uses the stable public surface only. Never link it: it is not a library.
#include "data_tamer/channel.hpp"
#include "data_tamer/custom_types.hpp"
#include "data_tamer/data_sink.hpp"
#include "data_tamer/data_tamer.hpp"
#include "data_tamer/logged_value.hpp"
#include "data_tamer/sinks/mcap_ring_sink.hpp"
#include "data_tamer/sinks/mcap_sink.hpp"
#include "data_tamer/values.hpp"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace abi_probe_types
{
struct Point
{
  double x = 0;
  double y = 0;
};

template <typename AddField>
std::string_view TypeDefinition(Point& point, AddField& add)
{
  add("x", &point.x);
  add("y", &point.y);
  return "ProbePoint";
}
}  // namespace abi_probe_types

namespace DataTamer
{
namespace abi_probe
{

class ProbeSink : public DataSink
{
public:
  ~ProbeSink() override;

protected:
  void onSchema(const Schema& schema) override;
  void onSnapshot(const SnapshotRef& snapshot) override;
  void onStop() override;
  void onStart() override;
};

ProbeSink::~ProbeSink() = default;
void ProbeSink::onSchema(const Schema&) {}
void ProbeSink::onSnapshot(const SnapshotRef&) {}
void ProbeSink::onStop() {}
void ProbeSink::onStart() {}

class ProbeSerializer : public CustomSerializer
{
public:
  ~ProbeSerializer() override;
  const std::string& typeName() const override;
  std::optional<CustomSchema> typeSchema() const override;
  size_t serializedSize(const void* instance) const override;
  bool isFixedSize() const override;
  void serialize(const void* instance, SerializeMe::SpanBytes& dest) const override;

private:
  std::string name_ = "probe";
};

ProbeSerializer::~ProbeSerializer() = default;
const std::string& ProbeSerializer::typeName() const
{
  return name_;
}
std::optional<CustomSchema> ProbeSerializer::typeSchema() const
{
  return std::nullopt;
}
size_t ProbeSerializer::serializedSize(const void*) const
{
  return sizeof(double);
}
bool ProbeSerializer::isFixedSize() const
{
  return true;
}
void ProbeSerializer::serialize(const void*, SerializeMe::SpanBytes&) const {}

/// Registration, logged values and write guards, instantiated as a consumer would.
void probeChannel(LogChannel& channel, ProbeSerializer& serializer)
{
  double scalar = 0;
  std::array<double, 3> array{};
  RegistrationID id = channel.registerValue("scalar", &scalar);
  channel.registerValue("array", &array);
  abi_probe_types::Point point;
  channel.registerValue("point", &point);
  channel.registerCustomValue("custom", &point, std::make_shared<ProbeSerializer>());
  channel.setEnabled(id, true);

  auto logged = channel.createLoggedValue<double>("logged", 1.0);
  logged->set(2.0);
  scalar = logged->get();
  logged->setEnabled(false);

  auto vector = channel.createLoggedValue<std::vector<double>>("vector");
  {
    auto guard = vector->getMutablePtr();
    guard->push_back(1.0);
  }
  {
    auto guard = vector->getConstPtr();
    scalar = static_cast<double>(guard->size());
  }
  {
    auto transaction = channel.scopedWrite();
    (void)transaction;
  }
  (void)serializer.serializedSize(&scalar);
}

/// Values that cross the library boundary by value.
Snapshot probeSnapshotCopy(const SnapshotRef& ref)
{
  return *ref;
}
/// The Stats structs are built inline from exported getters and never cross the
/// boundary, so they stay out of every signature here: they may gain fields.
uint64_t probeStats(const LogChannel& channel, const SinkWorker& worker,
                    const MCAPRingSink& ring)
{
  return channel.stats().attempts + worker.stats().delivered + ring.stats().dumps_written;
}
std::shared_ptr<SinkWorker> probeRingSink(MCAPRingOptions options)
{
  return MCAPRingSink::create(std::move(options));
}
std::shared_ptr<SinkWorker> probeSinks(const std::string& path)
{
  return MCAPSink::create(path);
}
size_t probeDump(const MCAPRingDump& dump)
{
  return dump.messages;
}
ValuePtr probeValuePtr(const double* value)
{
  return ValuePtr(value);
}
std::shared_ptr<SinkWorker> probeWorker()
{
  return SinkWorker::create<ProbeSink>();
}
ChannelDefaults::Sizes probeChannelDefaults(const ChannelDefaults& defaults)
{
  return defaults.resolve();
}
size_t probeRegistry(ChannelsRegistry& registry, TypesRegistry& types)
{
  registry.setChannelDefaults(ChannelDefaults{});
  auto channel = registry.getChannel("probe");
  (void)types.getSerializer<abi_probe_types::Point>();
  return channel->getNumberOfSinks();
}

}  // namespace abi_probe
}  // namespace DataTamer
