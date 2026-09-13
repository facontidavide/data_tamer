#include "data_tamer/channel.hpp"
#include "data_tamer/details/snapshot_pool.hpp"

#include "test_sinks.hpp"

#include <gtest/gtest.h>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <optional>
#include <future>
#include <thread>

using namespace DataTamer;
using DataTamerTest::Attached;

namespace
{
struct CustomValue
{
  uint64_t value = 42;
};
struct RegisteredCustom
{
  uint64_t value = 42;
};
template <class AddField>
std::string_view TypeDefinition(RegisteredCustom& value, AddField& add)
{
  add("value", &value.value);
  return "RegisteredCustom";
}
struct RejectedCustom
{
  uint64_t value = 42;
};
template <class AddField>
std::string_view TypeDefinition(RejectedCustom& value, AddField& add)
{
  add("value", &value.value);
  return "RejectedCustom";
}

struct Gate
{
  std::mutex mutex;
  std::condition_variable cv;
  bool entered = false;
  bool released = false;
  void pause()
  {
    std::unique_lock lock(mutex);
    entered = true;
    cv.notify_all();
    cv.wait(lock, [&] { return released; });
  }
  bool wait()
  {
    std::unique_lock lock(mutex);
    return cv.wait_for(lock, std::chrono::seconds(5), [&] { return entered; });
  }
  void release()
  {
    std::lock_guard lock(mutex);
    released = true;
    cv.notify_all();
  }
};

class PausedSerializer : public CustomSerializer
{
public:
  mutable Gate* gate = nullptr;
  bool throw_size = false;
  bool throw_serialize = false;
  bool throw_schema = false;
  mutable size_t size_calls = 0;
  std::optional<CustomSchema> typeSchema() const override
  {
    if(throw_schema)
      throw std::runtime_error("schema");
    return std::nullopt;
  }
  const std::string& typeName() const override
  {
    static const std::string name = "CustomValue";
    return name;
  }
  bool isFixedSize() const override { return true; }
  size_t serializedSize(const void*) const override
  {
    ++size_calls;
    if(gate)
      gate->pause();
    if(throw_size)
      throw std::runtime_error("size");
    return 8;
  }
  void serialize(const void* source, SerializeMe::SpanBytes& bytes) const override
  {
    if(throw_serialize)
      throw std::runtime_error("serialize");
    const auto value = static_cast<const CustomValue*>(source)->value;
    std::memcpy(bytes.data(), &value, 8);
    bytes.trimFront(8);
  }
};

class ControlSink : public DataSink
{
public:
  Gate* add_gate = nullptr;
  bool reject_schema = false;
  size_t registrations = 0;
  Schema schema;
  std::vector<Snapshot> snapshots;
  std::function<void(const SnapshotRef&)> on_store;
  void onSchema(const Schema& value) override
  {
    if(add_gate)
      add_gate->pause();
    if(reject_schema)
      throw std::runtime_error("schema");
    ++registrations;
    schema = value;
  }
  void onSnapshot(const SnapshotRef& value) override
  {
    snapshots.push_back(*value);
    if(on_store)
      on_store(value);
  }
};

/// Manual delivery: snapshots reach the sink only when the test drains it.
Attached<ControlSink> controlSink()
{
  return DataTamerTest::manual<ControlSink>();
}

void checkPayloads(const ControlSink& sink, size_t fields)
{
  for(const auto& snapshot : sink.snapshots)
  {
    size_t offset = 0;
    for(size_t i = 0; i < fields; ++i)
    {
      if(GetBit(snapshot.active_mask, i))
      {
        ASSERT_GE(snapshot.payload.size(), offset + 8);
        uint64_t value = 0;
        std::memcpy(&value, snapshot.payload.data() + offset, 8);
        EXPECT_EQ(value, 42u);
        offset += 8;
      }
    }
    EXPECT_EQ(snapshot.payload.size(), offset);
  }
}
}  // namespace

TEST(ChannelControl, EightSinksDuplicateAndRemoval)
{
  auto channel = LogChannel::create("control");
  uint64_t value = 42;
  channel->registerValue("value", &value);
  std::vector<Attached<ControlSink>> sinks;
  for(int i = 0; i < 8; ++i)
  {
    sinks.push_back(controlSink());
    channel->addDataSink(sinks.back());
  }
  channel->addDataSink(sinks.front());
  EXPECT_EQ(channel->getNumberOfSinks(), 8u);
  EXPECT_THROW(channel->addDataSink(controlSink()), std::runtime_error);
  ASSERT_TRUE(channel->takeSnapshot());
  for(auto& sink : sinks)
  {
    sink.drain();
    EXPECT_EQ(sink->registrations, 1u);
    EXPECT_EQ(sink->schema.hash, channel->getSchema().hash);
    EXPECT_EQ(sink->snapshots.size(), 1u);
    checkPayloads(*sink, 1);
  }
  channel->removeDataSink(sinks.front());
  channel->removeDataSink(sinks.front());
  ASSERT_TRUE(channel->takeSnapshot());
  sinks.front().drain();
  EXPECT_EQ(sinks.front()->snapshots.size(), 1u);
  EXPECT_EQ(channel->getNumberOfSinks(), 7u);
}

TEST(ChannelControl, RejectsNullSink)
{
  auto channel = LogChannel::create("control");
  EXPECT_THROW(channel->addDataSink(nullptr), std::invalid_argument);
  EXPECT_EQ(channel->getNumberOfSinks(), 0u);
}

TEST(ChannelControl, FirstCallWithoutSinksFreezesSchema)
{
  auto channel = LogChannel::create("control");
  uint64_t value = 42;
  channel->registerValue("value", &value);
  EXPECT_FALSE(channel->takeSnapshot());
  EXPECT_THROW(channel->registerValue("late", &value), std::runtime_error);
}

TEST(ChannelControl, StaleIdCannotResurrectDetachedValueAndTypesRemainChecked)
{
  auto channel = LogChannel::create("control");
  auto sink = controlSink();
  auto value = std::make_unique<uint64_t>(42);
  auto id = channel->registerValue("value", value.get());
  channel->addDataSink(sink);
  ASSERT_TRUE(channel->takeSnapshot());
  channel->unregister(id);
  // Keep storage alive until after this baseline regression assertion.
  channel->setEnabled(id, true);
  EXPECT_FALSE(channel->sharedState()->isEnabled(id.first_index));
  ASSERT_TRUE(channel->takeSnapshot());
  sink.drain();
  EXPECT_FALSE(GetBit(sink->snapshots.back().active_mask, 0));
  EXPECT_TRUE(sink->snapshots.back().payload.empty());
  EXPECT_EQ(channel->getActiveFlags(), sink->snapshots.back().active_mask);
  value.reset();
  uint32_t changed = 42;
  EXPECT_THROW(channel->registerValue("value", &changed), std::runtime_error);
  uint64_t replacement = 42;
  auto next = channel->registerValue("value", &replacement);
  EXPECT_EQ(next.first_index, id.first_index);
  ASSERT_TRUE(channel->takeSnapshot());
  sink.drain();
  checkPayloads(*sink, 1);
}

TEST(ChannelControl, UnregisterWaitsForPausedReaderBeforeValueDestruction)
{
  auto channel = LogChannel::create("control");
  auto sink = controlSink();
  auto value = std::make_unique<CustomValue>();
  auto serializer = std::make_shared<PausedSerializer>();
  auto id = channel->registerCustomValue("value", value.get(), serializer);
  channel->addDataSink(sink);
  ASSERT_TRUE(channel->takeSnapshot());
  Gate gate;
  serializer->gate = &gate;
  auto snapshot = std::async(std::launch::async, [&] { return channel->takeSnapshot(); });
  EXPECT_TRUE(gate.wait());
  std::atomic<bool> returned{ false };
  auto removal = std::async(std::launch::async, [&] {
    channel->unregister(id);
    returned = true;
  });
  // Observing cleared liveness establishes that removal reached its publication.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while(channel->sharedState()->isEnabled(id.first_index) &&
        std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  EXPECT_FALSE(channel->sharedState()->isEnabled(id.first_index));
  EXPECT_FALSE(returned.load());
  gate.release();
  EXPECT_TRUE(snapshot.get());
  removal.get();
  value.reset();
  serializer->gate = nullptr;
  channel->setEnabled(id, true);
  EXPECT_TRUE(channel->takeSnapshot());
  sink.drain();
  EXPECT_FALSE(GetBit(sink->snapshots.back().active_mask, 0));
  EXPECT_TRUE(sink->snapshots.back().payload.empty());
}

TEST(ChannelControl, BlockedAddChannelDoesNotBlockExistingSnapshots)
{
  auto channel = LogChannel::create("control");
  auto existing = controlSink();
  uint64_t value = 42;
  channel->registerValue("value", &value);
  channel->addDataSink(existing);
  ASSERT_TRUE(channel->takeSnapshot());
  Gate gate;
  auto added = controlSink();
  added->add_gate = &gate;
  auto add = std::async(std::launch::async, [&] { channel->addDataSink(added); });
  EXPECT_TRUE(gate.wait());
  auto snapshot = std::async(std::launch::async, [&] { return channel->takeSnapshot(); });
  EXPECT_EQ(snapshot.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  gate.release();
  add.get();
  EXPECT_TRUE(snapshot.get());
  existing.drain();
  EXPECT_EQ(existing->snapshots.size(), 2u);
  EXPECT_TRUE(channel->takeSnapshot());
  added.drain();
  EXPECT_EQ(added->registrations, 1u);
  EXPECT_FALSE(added->snapshots.empty());
}

// The paused serializer parks the reader inside its epoch, after loading the sink
// links and before pushing: removal must wait, and the push must still reach the
// unpublished link. Queued and retained references then outlive channel and sink.
TEST(ChannelControl, RemovalWaitsForPausedReaderAndReferencesSurvive)
{
  auto channel = LogChannel::create("control");
  auto sink = controlSink();
  CustomValue value;
  auto serializer = std::make_shared<PausedSerializer>();
  channel->registerCustomValue("value", &value, serializer);
  channel->addDataSink(sink);
  ASSERT_TRUE(channel->takeSnapshot());
  Gate gate;
  serializer->gate = &gate;
  auto snapshot = std::async(std::launch::async, [&] { return channel->takeSnapshot(); });
  EXPECT_TRUE(gate.wait());
  auto removal = std::async(std::launch::async, [&] { channel->removeDataSink(sink); });
  EXPECT_EQ(removal.wait_for(std::chrono::milliseconds(50)), std::future_status::timeout);
  gate.release();
  EXPECT_TRUE(snapshot.get());
  removal.get();
  EXPECT_EQ(channel->getNumberOfSinks(), 0u);
  channel.reset();
  SnapshotRef retained;
  sink->on_store = [&](const SnapshotRef& ref) { retained = ref.clone(); };
  sink.drain();
  EXPECT_EQ(sink->snapshots.size(), 2u);
  checkPayloads(*sink, 1);
  sink.worker.reset();
  ASSERT_TRUE(retained);
  EXPECT_EQ(retained->payload.size(), 8u);
}

TEST(ChannelControl, FailedFreezeRetriesAllLinksAndStillFreezesSchema)
{
  auto channel = LogChannel::create("control");
  auto good = controlSink();
  auto failing = controlSink();
  uint64_t value = 42;
  channel->registerValue("value", &value);
  channel->addDataSink(failing);
  channel->addDataSink(good);  // Visited first: a retry must not register it twice.
  failing->reject_schema = true;
  EXPECT_THROW(channel->takeSnapshot(), std::runtime_error);
  EXPECT_THROW(channel->registerValue("late", &value), std::runtime_error);
  failing->reject_schema = false;
  EXPECT_TRUE(channel->takeSnapshot());
  EXPECT_EQ(good->registrations, 1u);
  EXPECT_EQ(failing->registrations, 1u);
  auto rejected = controlSink();
  rejected->reject_schema = true;
  EXPECT_THROW(channel->addDataSink(rejected), std::runtime_error);
  EXPECT_EQ(channel->getNumberOfSinks(), 2u);
  EXPECT_TRUE(channel->takeSnapshot());
  good.drain();
  failing.drain();
  EXPECT_EQ(good->snapshots.size(), 2u);
  EXPECT_EQ(failing->snapshots.size(), 2u);
}

TEST(ChannelControl, RejectedCustomRegistrationDoesNotMutateFrozenSchema)
{
  auto channel = LogChannel::create("control");
  auto sink = controlSink();
  RegisteredCustom value;
  auto id = channel->registerValue("value", &value);
  channel->addDataSink(sink);
  ASSERT_TRUE(channel->takeSnapshot());
  const auto before = channel->getSchema();
  RejectedCustom rejected;
  EXPECT_THROW(channel->registerValue("late", &rejected), std::runtime_error);
  channel->unregister(id);
  EXPECT_THROW(channel->registerValue("value", &rejected), std::runtime_error);
  const auto after = channel->getSchema();
  EXPECT_EQ(ToStr(after), ToStr(before));
  EXPECT_EQ(after.hash, before.hash);
  EXPECT_EQ(after.fields.size(), before.fields.size());
  EXPECT_EQ(after.custom_types.size(), before.custom_types.size());
  EXPECT_EQ(after.custom_types.count("RejectedCustom"), 0u);
  EXPECT_EQ(after.custom_schemas.size(), before.custom_schemas.size());
  EXPECT_NO_THROW(channel->registerValue("value", &value));
}

TEST(ChannelControl, ConcurrentChurnTogglesAndSinkChangesPreservePayloads)
{
  auto channel = LogChannel::create("control");
  auto stable = controlSink();
  auto changing = controlSink();
  auto logged = channel->createLoggedValue<uint64_t>("logged", 42);
  RegisteredCustom value;
  auto custom_id = channel->registerValue("custom", &value);
  RegisteredCustom second_value;
  auto second_id = channel->registerValue("second_custom", &second_value);
  channel->addDataSink(stable);
  ASSERT_TRUE(channel->takeSnapshot());
  // Reuse the same registered type from two independent controller threads.
  std::atomic<bool> done{ false };
  std::thread toggler([&] {
    while(!done)
    {
      channel->setEnabled({ 0, 3 }, false);
      channel->setEnabled({ 0, 3 }, true);
    }
  });
  std::thread second_control([&] {
    for(int i = 0; i < 500; ++i)
    {
      channel->unregister(second_id);
      second_id = channel->registerValue("second_custom", &second_value);
    }
  });
  std::thread control([&] {
    for(int i = 0; i < 500; ++i)
    {
      logged.reset();
      logged = channel->createLoggedValue<uint64_t>("logged", 42);
      channel->unregister(custom_id);
      custom_id = channel->registerValue("custom", &value);
      channel->addDataSink(changing);
      channel->removeDataSink(changing);
    }
    done = true;
  });
  do
  {
    channel->takeSnapshot();
    stable.drain();
    changing.drain();
  } while(!done);
  control.join();
  second_control.join();
  toggler.join();
  stable.drain();
  changing.drain();
  checkPayloads(*stable, 3);
  checkPayloads(*changing, 3);
  EXPECT_FALSE(stable->snapshots.empty());
}

TEST(ChannelControl, SerializerExceptionsLeaveEpochAndPoolReusable)
{
  for(bool size_pass : { false, true })
  {
    auto channel = LogChannel::create("control");
    auto sink = controlSink();
    CustomValue value;
    auto serializer = std::make_shared<PausedSerializer>();
    auto id = channel->registerCustomValue("value", &value, serializer);
    channel->addDataSink(sink);
    channel->setPoolCapacity(1);  // a slot leaked by the throw would fail the next call
    ASSERT_TRUE(channel->takeSnapshot());
    sink.drain();
    serializer->throw_size = size_pass;
    serializer->throw_serialize = !size_pass;
    EXPECT_THROW(channel->takeSnapshot(), std::runtime_error);
    serializer->throw_size = serializer->throw_serialize = false;
    EXPECT_TRUE(channel->takeSnapshot());
    sink.drain();
    channel->unregister(id);
    EXPECT_TRUE(channel->takeSnapshot());
    sink.drain();
    EXPECT_EQ(channel->poolExhausted(), 0u);
    EXPECT_EQ(sink->snapshots.size(), 3u);
    checkPayloads(*sink, 1);
  }
}

TEST(ChannelControl, ExhaustedPoolDoesNotCallSerializer)
{
  auto channel = LogChannel::create("control");
  auto sink = controlSink();
  CustomValue value;
  auto serializer = std::make_shared<PausedSerializer>();
  channel->registerCustomValue("value", &value, serializer);
  channel->addDataSink(sink);
  for(size_t i = 0; i < SnapshotPool::kDefaultCapacity; ++i)
    ASSERT_TRUE(channel->takeSnapshot());
  const auto calls = serializer->size_calls;
  EXPECT_FALSE(channel->takeSnapshot());
  EXPECT_EQ(serializer->size_calls, calls);
}

TEST(ChannelControl, FailedRegistrationLeavesTheChannelUnchanged)
{
  auto channel = LogChannel::create("control");
  CustomValue value;
  auto serializer = std::make_shared<PausedSerializer>();
  serializer->throw_schema = true;
  EXPECT_THROW(channel->registerCustomValue("value", &value, serializer),
               std::runtime_error);
  EXPECT_TRUE(channel->getSchema().fields.empty());
  serializer->throw_schema = false;
  EXPECT_NO_THROW(channel->registerCustomValue("value", &value, serializer));
  EXPECT_EQ(channel->getSchema().fields.size(), 1u);
  EXPECT_THROW(channel->registerCustomValue("other", &value, CustomSerializer::Ptr{}),
               std::invalid_argument);
}
