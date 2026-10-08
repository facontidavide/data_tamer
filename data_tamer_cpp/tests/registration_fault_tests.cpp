// Registration under allocation failure. This program replaces operator new so that
// the Nth allocation of the calling thread throws std::bad_alloc. For every N, a
// registration must complete or leave the channel as it was, and the channel must go
// on working. It is a program of its own because operator new is replaced for all of it.
#include "data_tamer/channel.hpp"
#include "data_tamer/sinks/dummy_sink.hpp"
#include "data_tamer_parser/data_tamer_parser.hpp"

#include "test_sinks.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace DataTamer;

namespace
{
struct FaultState
{
  static thread_local long countdown;  // allocations until the failing one; 0: off
  static thread_local bool fired;
};
thread_local long FaultState::countdown = 0;
thread_local bool FaultState::fired = false;

// Fails the nth allocation made by this thread while it lives.
class AllocationFault
{
public:
  explicit AllocationFault(long nth)
  {
    FaultState::fired = false;
    FaultState::countdown = nth;
  }
  ~AllocationFault() { disarm(); }
  AllocationFault(const AllocationFault&) = delete;
  AllocationFault& operator=(const AllocationFault&) = delete;

  void disarm() { FaultState::countdown = 0; }
  bool fired() const { return FaultState::fired; }
};

void* allocate(std::size_t size)
{
  if(FaultState::countdown > 0 && --FaultState::countdown == 0)
  {
    FaultState::fired = true;
    throw std::bad_alloc();
  }
  if(void* p = std::malloc(size == 0 ? 1 : size))
  {
    return p;
  }
  throw std::bad_alloc();
}

void* allocateOrNull(std::size_t size) noexcept
{
  try
  {
    return allocate(size);
  }
  catch(...)
  {
    return nullptr;
  }
}

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

struct Field
{
  std::string name;
  double value;
};

struct Scenario
{
  std::string label;
  std::function<RegistrationID(LogChannel&)> register_target;
  size_t target_bytes;         // what the target adds to the payload
  std::vector<Field> decoded;  // what a decoder reads back (empty: not decodable)
};

std::vector<Scenario> scenarios()
{
  return {
    { "scalar",
      [](LogChannel& c) { return c.registerValue("target", &g_scalar); },
      8,
      { { "target", 42.0 } } },
    { "atomic",
      [](LogChannel& c) { return c.registerValue("target", &g_atomic); },
      4,
      { { "target", 7.0 } } },
    { "vector",
      [](LogChannel& c) { return c.registerValue("target", &g_vector); },
      4 + 16,
      { { "target[0]", 1.5 }, { "target[1]", 2.5 } } },
    { "array",
      [](LogChannel& c) { return c.registerValue("target", &g_array); },
      8,
      { { "target[0]", 3.5 }, { "target[1]", 4.5 } } },
    { "custom type",
      [](LogChannel& c) { return c.registerValue("target", &g_reading); },
      8,
      { { "target/value", 9.0 } } },
    { "nested custom types",
      [](LogChannel& c) { return c.registerValue("target", &g_outer); },
      8 + 4 + 16,
      { { "target/first/a", 1.0 },
        { "target/rest[0]/a", 2.0 },
        { "target/rest[1]/a", 3.0 } } },
    { "custom serializer with a schema",
      [](LogChannel& c) {
        return c.registerCustomValue("target", &g_blob, g_blob_serializer);
      },
      4,
      {} },
  };
}

constexpr long kMaxAllocations = 1000;

// The channel registered the target and still works: the id is live, the schema is
// consistent, and a snapshot holds the existing values and the target.
void ExpectChannelWorks(LogChannel& channel, const Scenario& scenario,
                        size_t existing_count, const RegistrationID& id)
{
  EXPECT_TRUE(channel.isEnabled(id));
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

  std::vector<Field> expected;
  for(size_t i = 0; i < existing_count; ++i)
  {
    expected.push_back({ "existing/" + std::to_string(i), double(i) });
  }
  expected.insert(expected.end(), scenario.decoded.begin(), scenario.decoded.end());

  const auto parsed = DataTamerParser::BuildSchemaFromText(ToStr(schema));
  const DataTamerParser::SnapshotView view{
    snapshot.schema_hash,
    0,
    { snapshot.active_mask.data(), snapshot.active_mask.size() },
    { snapshot.payload.data(), snapshot.payload.size() }
  };
  std::vector<Field> actual;
  ASSERT_TRUE(DataTamerParser::ParseSnapshot(
      parsed, view, [&](const std::string& name, const DataTamerParser::VarNumber& v) {
        actual.push_back({ name, std::visit([](auto x) { return double(x); }, v) });
      }));
  ASSERT_EQ(actual.size(), expected.size());
  for(size_t i = 0; i < expected.size(); ++i)
  {
    EXPECT_EQ(actual[i].name, expected[i].name);
    EXPECT_EQ(actual[i].value, expected[i].value);
  }
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
    std::optional<RegistrationID> id;
    {
      AllocationFault fault(nth);
      try
      {
        id = scenario.register_target(*channel);
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
      ASSERT_TRUE(id.has_value());
      ExpectChannelWorks(*channel, scenario, existing_count, *id);
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
    std::optional<RegistrationID> again;
    ASSERT_NO_THROW(again = scenario.register_target(*channel));
    ExpectChannelWorks(*channel, scenario, existing_count, *again);
    if(::testing::Test::HasFailure())
    {
      return;  // the first broken allocation is the one to look at
    }
  }
  FAIL() << "the registration still allocated at allocation " << kMaxAllocations;
}
}  // namespace

void* operator new(std::size_t size)
{
  return allocate(size);
}
void* operator new[](std::size_t size)
{
  return allocate(size);
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
  return allocateOrNull(size);
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
  return allocateOrNull(size);
}
void operator delete(void* p) noexcept
{
  std::free(p);
}
void operator delete[](void* p) noexcept
{
  std::free(p);
}
void operator delete(void* p, std::size_t) noexcept
{
  std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept
{
  std::free(p);
}

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
  AllocationFault fault(1);
  RegistrationID again;
  EXPECT_NO_THROW(again = channel->registerValue("value", &value));
  EXPECT_FALSE(fault.fired());
  fault.disarm();
  EXPECT_TRUE(channel->isEnabled(again));
}
