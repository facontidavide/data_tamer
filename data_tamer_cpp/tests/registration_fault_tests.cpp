// Registration under allocation failure. AllocCounter::FailNth makes the Nth allocation
// of the calling thread throw std::bad_alloc. For every N, a registration must complete
// or leave the channel as it was, and the channel must go on working.
#include "data_tamer/channel.hpp"
#include "data_tamer/sinks/dummy_sink.hpp"

#include "alloc_counter.hpp"
#include "decode_utils.hpp"
#include "test_sinks.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstring>
#include <functional>
#include <map>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace DataTamer;

namespace
{
using DataTamerTest::AllocCounter;

struct Reading
{
  double value = 0;
};
template <typename AddField>
std::string_view TypeDefinition(Reading& reading, AddField& add)
{
  add("value", &reading.value);
  return "Reading";
}

struct Inner
{
  double a = 0;
};
template <typename AddField>
std::string_view TypeDefinition(Inner& inner, AddField& add)
{
  add("a", &inner.a);
  return "Inner";
}

struct Outer
{
  Inner first;
  std::vector<Inner> rest;
};
template <typename AddField>
std::string_view TypeDefinition(Outer& outer, AddField& add)
{
  add("first", &outer.first);
  add("rest", &outer.rest);
  return "Outer";
}

struct Blob
{
  uint32_t bits = 0xCAFE;
};

// A serializer that brings its own schema text, so that registering it adds an entry to
// Schema::custom_schemas.
class BlobSerializer : public CustomSerializer
{
public:
  const std::string& typeName() const override
  {
    static const std::string name = "Blob";
    return name;
  }
  std::optional<CustomSchema> typeSchema() const override
  {
    return CustomSchema{ "text", "uint32 bits" };
  }
  size_t serializedSize(const void*) const override { return 4; }
  bool isFixedSize() const override { return true; }
  void serialize(const void* instance, SerializeMe::SpanBytes& bytes) const override
  {
    std::memcpy(bytes.data(), &static_cast<const Blob*>(instance)->bits, 4);
    bytes.trimFront(4);
  }
};

// What gets registered: it lives on after the channel, which only borrows it.
double g_scalar = 42.0;
std::atomic<int32_t> g_atomic{ 7 };
std::vector<double> g_vector{ 1.5, 2.5 };
std::array<float, 2> g_array{ 3.5f, 4.5f };
Reading g_reading{ 9.0 };
Outer g_outer{ Inner{ 1.0 }, { Inner{ 2.0 }, Inner{ 3.0 } } };
Blob g_blob;
const auto g_blob_serializer = std::make_shared<BlobSerializer>();

// What a registration returns, kept outside the code that fails allocations.
struct Target
{
  std::optional<RegistrationID> id;
  std::shared_ptr<LoggedValue<double>> logged;

  bool isEnabled(const LogChannel& channel) const
  {
    return logged ? logged->isEnabled() : id && channel.isEnabled(*id);
  }
};

struct Scenario
{
  std::string label;
  std::function<void(LogChannel&, Target&)> register_target;
  size_t target_bytes;                    // what the target adds to the payload
  std::map<std::string, double> decoded;  // what a decoder reads back (empty: none)
};

std::vector<Scenario> scenarios()
{
  return {
    { "scalar",
      [](LogChannel& c, Target& t) { t.id = c.registerValue("target", &g_scalar); },
      8,
      { { "target", 42.0 } } },
    { "atomic",
      [](LogChannel& c, Target& t) { t.id = c.registerValue("target", &g_atomic); },
      4,
      { { "target", 7.0 } } },
    { "vector",
      [](LogChannel& c, Target& t) { t.id = c.registerValue("target", &g_vector); },
      4 + 16,
      { { "target[0]", 1.5 }, { "target[1]", 2.5 } } },
    { "array",
      [](LogChannel& c, Target& t) { t.id = c.registerValue("target", &g_array); },
      8,
      { { "target[0]", 3.5 }, { "target[1]", 4.5 } } },
    { "custom type",
      [](LogChannel& c, Target& t) { t.id = c.registerValue("target", &g_reading); },
      8,
      { { "target/value", 9.0 } } },
    { "nested custom types",
      [](LogChannel& c, Target& t) { t.id = c.registerValue("target", &g_outer); },
      8 + 4 + 16,
      { { "target/first/a", 1.0 },
        { "target/rest[0]/a", 2.0 },
        { "target/rest[1]/a", 3.0 } } },
    { "logged value",
      [](LogChannel& c, Target& t) {
        t.logged = c.createLoggedValue<double>("target", 42.0);
      },
      8,
      { { "target", 42.0 } } },
    { "custom serializer with a schema",
      [](LogChannel& c, Target& t) {
        t.id = c.registerCustomValue("target", &g_blob, g_blob_serializer);
      },
      4,
      {} },
  };
}

constexpr long kMaxAllocations = 1000;

// The channel registered the target and still works: the id is live, the schema is
// consistent, and a snapshot holds the existing values and the target.
void ExpectChannelWorks(LogChannel& channel, const Scenario& scenario,
                        size_t existing_count, const Target& target)
{
  EXPECT_TRUE(target.isEnabled(channel));
  const auto schema = channel.getSchema();
  EXPECT_EQ(schema.hash, ComputeSchemaHash(schema));
  EXPECT_EQ(schema.fields.size(), existing_count + 1);

  DataTamerTest::Attached<DummySink> sink = DataTamerTest::manual<DummySink>();
  channel.addDataSink(sink);
  ASSERT_EQ(channel.takeSnapshot(), SnapshotResult::ok);
  sink.drain();
  const auto snapshot = sink->latestSnapshot();
  EXPECT_EQ(snapshot.payload.size(), existing_count * 8 + scenario.target_bytes);
  if(scenario.decoded.empty())
  {
    return;
  }

  auto expected = scenario.decoded;
  for(size_t i = 0; i < existing_count; ++i)
  {
    expected["existing/" + std::to_string(i)] = double(i);
  }
  EXPECT_EQ(DataTamerTest::decode(channel, snapshot), expected);
}

// Fails the nth allocation of one registration, for n = 1, 2, ... until the
// registration needs fewer allocations than n.
void FailEveryAllocation(const Scenario& scenario, size_t existing_count)
{
  SCOPED_TRACE(scenario.label + " after " + std::to_string(existing_count) + " values");
  for(long nth = 1; nth < kMaxAllocations; ++nth)
  {
    SCOPED_TRACE("allocation " + std::to_string(nth));
    std::vector<double> existing(existing_count);
    auto channel = LogChannel::create("fault");
    for(size_t i = 0; i < existing_count; ++i)
    {
      existing[i] = double(i);
      channel->registerValue("existing/" + std::to_string(i), &existing[i]);
    }
    const std::string text_before = ToStr(channel->getSchema());
    const auto hash_before = channel->getSchema().hash;

    bool threw = false;
    bool fired = false;
    std::string other_error;
    Target target;
    {
      AllocCounter::FailNth fault(nth);
      try
      {
        scenario.register_target(*channel, target);
      }
      catch(const std::bad_alloc&)
      {
        threw = true;
      }
      catch(const std::exception& error)
      {
        fault.disarm();
        other_error = error.what();
      }
      fired = fault.fired();
    }
    ASSERT_EQ(other_error, "") << "only bad_alloc is injected";
    if(!threw)
    {
      ExpectChannelWorks(*channel, scenario, existing_count, target);
      if(!fired)
      {
        return;  // the registration made fewer than nth allocations: all were failed
      }
      continue;
    }

    // The failed registration left the channel as it was ...
    EXPECT_EQ(ToStr(channel->getSchema()), text_before);
    EXPECT_EQ(channel->getSchema().hash, hash_before);
    // ... and registering again works, with the injection off.
    Target again;
    ASSERT_NO_THROW(scenario.register_target(*channel, again));
    ExpectChannelWorks(*channel, scenario, existing_count, again);
    if(::testing::Test::HasFailure())
    {
      return;  // the first broken allocation is the one to look at
    }
  }
  FAIL() << "the registration still allocated at allocation " << kMaxAllocations;
}
}  // namespace

// The first registration allocates the first block of flag words, the 65th the second.
TEST(RegistrationFault, EveryAllocationFailureLeavesTheChannelUnchanged)
{
  for(const auto& scenario : scenarios())
  {
    for(const size_t existing_count : { size_t(0), size_t(1), size_t(64) })
    {
      FailEveryAllocation(scenario, existing_count);
      if(::testing::Test::HasFailure())
      {
        return;
      }
    }
  }
}

// A name registered again after unregister() reuses its slot and allocates nothing.
TEST(RegistrationFault, ReRegistrationNeedsNoAllocation)
{
  auto channel = LogChannel::create("fault");
  double value = 1.0;
  const auto id = channel->registerValue("value", &value);
  channel->unregister(id);
  AllocCounter::FailNth fault(1);
  RegistrationID again;
  EXPECT_NO_THROW(again = channel->registerValue("value", &value));
  EXPECT_FALSE(fault.fired());
  fault.disarm();
  EXPECT_TRUE(channel->isEnabled(again));
}
