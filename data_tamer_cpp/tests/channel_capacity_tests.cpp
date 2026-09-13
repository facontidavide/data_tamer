#include "data_tamer/channel.hpp"
#include "data_tamer/details/snapshot_pool.hpp"
#include "alloc_counter.hpp"
#include "test_sinks.hpp"

#include <gtest/gtest.h>
#include <chrono>
#include <cstring>
#include <limits>
#include <thread>

using namespace DataTamer;

namespace
{
class CapacitySink : public DataSink
{
public:
  SnapshotRef last;
  size_t received = 0;
  void onSchema(const Schema&) override {}
  void onSnapshot(const SnapshotRef& snapshot) override
  {
    ++received;
    last = snapshot.clone();
  }
};

/// Manual delivery, with the retained reference released after each drain.
struct CapacityWorker : DataTamerTest::Attached<CapacitySink>
{
  CapacityWorker() : Attached(DataTamerTest::manual<CapacitySink>().worker) {}
  void drain() const
  {
    worker->drain();
    sink->last.reset();
  }
  /// Deliver and keep the last reference, so the slot stays in use.
  void deliver() const { worker->drain(); }
};

struct SizedValue
{
  uint64_t value = 42;
};
class SizedSerializer : public CustomSerializer
{
public:
  size_t size = 8;
  mutable size_t calls = 0;
  bool throw_size = false, throw_serialize = false;
  const std::string& typeName() const override
  {
    static const std::string name = "SizedValue";
    return name;
  }
  bool isFixedSize() const override { return false; }
  size_t serializedSize(const void*) const override
  {
    ++calls;
    if(throw_size)
      throw std::runtime_error("size");
    return size;
  }
  void serialize(const void* source, SerializeMe::SpanBytes& bytes) const override
  {
    if(throw_serialize)
      throw std::runtime_error("serialize");
    const auto value = static_cast<const SizedValue*>(source)->value;
    std::memcpy(bytes.data(), &value, 8);
    bytes.trimFront(8);
  }
};
}  // namespace

TEST(ChannelCapacity, FreezeWithoutSinkReservesAndRejectsSetters)
{
  auto channel = LogChannel::create("capacity");
  auto value = channel->createLoggedValue<std::vector<double>>("value", { 1.0 });
  EXPECT_THROW(channel->setPoolCapacity(0), std::invalid_argument);
  EXPECT_THROW(channel->setPoolCapacity(std::numeric_limits<size_t>::max()),
               std::length_error);
  EXPECT_THROW(channel->setPayloadCapacity(std::numeric_limits<size_t>::max()),
               std::length_error);
  channel->setPoolCapacity(2);
  channel->setPayloadCapacity(0);  // Automatic minimum remains valid.
  channel->setStrictMode(true);
  EXPECT_FALSE(channel->takeSnapshot());
  EXPECT_TRUE(GetBit(channel->getActiveFlags(), 0));
  EXPECT_THROW(channel->setPoolCapacity(2), std::runtime_error);
  EXPECT_THROW(channel->setPayloadCapacity(256), std::runtime_error);
  value->set(std::vector<double>(1024, 3.0));
  CapacityWorker sink;
  channel->addDataSink(sink);
  EXPECT_FALSE(channel->takeSnapshot());  // Must use the no-sink freeze's reservation.
  EXPECT_EQ(channel->droppedOversize(), 1u);
  EXPECT_EQ(channel->payloadReallocations(), 0u);
}

TEST(ChannelCapacity, TwoSlotsExhaustBeforeSizingAndRecover)
{
  auto channel = LogChannel::create("capacity");
  CapacityWorker sink;
  auto serializer = std::make_shared<SizedSerializer>();
  SizedValue value;
  channel->registerCustomValue("value", &value, serializer);
  channel->setPoolCapacity(2);
  channel->addDataSink(sink);
  ASSERT_TRUE(channel->takeSnapshot());
  ASSERT_TRUE(channel->takeSnapshot());
  const auto calls = serializer->calls;
  EXPECT_FALSE(channel->takeSnapshot());
  EXPECT_EQ(serializer->calls, calls);
  EXPECT_EQ(channel->poolExhausted(), 1u);
  sink.drain();
  EXPECT_TRUE(channel->takeSnapshot());
  EXPECT_EQ(channel->droppedSnapshots(sink), 0u);
}

TEST(ChannelCapacity, StrictDropNonStrictGrowthAndRuntimeToggleUseEachSlotCapacity)
{
  auto channel = LogChannel::create("capacity");
  CapacityWorker sink;
  auto value = channel->createLoggedValue<std::vector<double>>("value", { 1.0 });
  channel->addDataSink(sink);
  channel->setPoolCapacity(2);
  channel->setPayloadCapacity(256);
  channel->setStrictMode(true);
  ASSERT_TRUE(channel->takeSnapshot());
  sink.drain();
  value->set(std::vector<double>(1024, 3.0));
  EXPECT_FALSE(channel->takeSnapshot());
  EXPECT_EQ(channel->droppedOversize(), 1u);
  EXPECT_EQ(channel->payloadReallocations(), 0u);
  channel->setStrictMode(false);
  ASSERT_TRUE(channel->takeSnapshot());
  EXPECT_EQ(channel->payloadReallocations(), 1u);
  sink.drain();
  channel->setStrictMode(true);
  EXPECT_FALSE(channel->takeSnapshot());  // Other slot is still small.
  ASSERT_TRUE(channel->takeSnapshot());   // Grown slot remains usable in strict mode.
  sink.deliver();
  ASSERT_TRUE(sink->last);
  EXPECT_EQ(sink->last->payload.size(), 8196u);
  uint32_t count = 0;
  double first = 0;
  std::memcpy(&count, sink->last->payload.data(), 4);
  std::memcpy(&first, sink->last->payload.data() + 4, 8);
  EXPECT_EQ(count, 1024u);
  EXPECT_EQ(first, 3.0);
  sink->last.reset();
  channel->setStrictMode(false);
  EXPECT_TRUE(channel->takeSnapshot());
  EXPECT_EQ(channel->payloadReallocations(), 2u);
  sink.drain();
  value->set(std::vector<double>(1500, 4.0));  // Doubled growth provides spare capacity.
  channel->setStrictMode(true);
  for(int i = 0; i < 4; ++i)
  {
    EXPECT_TRUE(channel->takeSnapshot());
    sink.drain();
  }
  EXPECT_EQ(channel->stats().dropped_oversize, 2u);
  EXPECT_EQ(channel->stats().payload_reallocations, 2u);
}

TEST(ChannelCapacity, ReservationCoversHintDoubleInitialSizeAndMinimum)
{
  for(size_t hint : { size_t(0), size_t(4096) })
  {
    auto channel = LogChannel::create("capacity");
    CapacityWorker sink;
    auto value = channel->createLoggedValue<std::vector<double>>(
        "value", std::vector<double>(100));
    channel->setPoolCapacity(2);
    channel->setPayloadCapacity(hint);
    channel->setStrictMode(true);
    channel->addDataSink(sink);
    ASSERT_TRUE(channel->takeSnapshot());
    sink.drain();
    value->set(std::vector<double>(hint ? 500 : 200));
    for(int i = 0; i < 4; ++i)
    {
      EXPECT_TRUE(channel->takeSnapshot());
      sink.drain();
    }
    EXPECT_EQ(channel->payloadReallocations(), 0u);
    EXPECT_EQ(channel->droppedOversize(), 0u);
  }
}

TEST(ChannelCapacity, DisabledOversizedAndDeadFieldsNeverSizeOrSerialize)
{
  auto channel = LogChannel::create("capacity");
  CapacityWorker sink;
  auto serializer = std::make_shared<SizedSerializer>();
  auto value = std::make_unique<SizedValue>();
  auto id = channel->registerCustomValue("value", value.get(), serializer);
  serializer->throw_size = serializer->throw_serialize = true;
  channel->setEnabled(id, false);
  channel->setStrictMode(true);
  channel->addDataSink(sink);
  ASSERT_TRUE(channel->takeSnapshot());
  channel->unregister(id);
  value.reset();
  channel->setEnabled(id, true);
  ASSERT_TRUE(channel->takeSnapshot());
  sink.deliver();
  ASSERT_TRUE(sink->last);
  EXPECT_FALSE(GetBit(sink->last->active_mask, 0));
  EXPECT_TRUE(sink->last->payload.empty());
  EXPECT_EQ(serializer->calls, 0u);
  EXPECT_EQ(channel->droppedOversize(), 0u);
}

TEST(ChannelCapacity, ExceptionsReleaseSingleSlotAndEpochEvenInStrictMode)
{
  for(bool strict : { false, true })
    for(bool sizing : { false, true })
    {
      auto channel = LogChannel::create("capacity");
      CapacityWorker sink;
      auto serializer = std::make_shared<SizedSerializer>();
      SizedValue value;
      auto id = channel->registerCustomValue("value", &value, serializer);
      channel->setPoolCapacity(1);
      channel->setStrictMode(strict);
      channel->addDataSink(sink);
      ASSERT_TRUE(channel->takeSnapshot());
      sink.drain();
      serializer->throw_size = sizing;
      serializer->throw_serialize = !sizing;
      EXPECT_THROW(channel->takeSnapshot(), std::runtime_error);
      serializer->throw_size = serializer->throw_serialize = false;
      channel->unregister(id);  // A leaked odd epoch would block here.
      channel->registerCustomValue("value", &value, serializer);
      EXPECT_TRUE(channel->takeSnapshot());
      EXPECT_EQ(channel->poolExhausted(), 0u);
    }
}

TEST(ChannelCapacity, ImpossibleSizesFailSafelyAndFreezeCanRetry)
{
  auto channel = LogChannel::create("capacity");
  CapacityWorker sink;
  auto serializer = std::make_shared<SizedSerializer>();
  SizedValue value;
  auto id = channel->registerCustomValue("value", &value, serializer);
  channel->registerCustomValue("second", &value, serializer);
  channel->setPoolCapacity(1);
  channel->addDataSink(sink);
  serializer->size = std::numeric_limits<size_t>::max();
  EXPECT_THROW(channel->takeSnapshot(), std::length_error);
  EXPECT_THROW(channel->setPayloadCapacity(256), std::runtime_error);
  serializer->size = 8;
  ASSERT_TRUE(channel->takeSnapshot());
  sink.drain();
  // Each size fits the byte vector, but their sum cannot be doubled.
  serializer->size = std::vector<uint8_t>().max_size() / 2;
  EXPECT_THROW(channel->takeSnapshot(), std::length_error);
  serializer->size = std::vector<uint8_t>().max_size();
  EXPECT_THROW(channel->takeSnapshot(), std::length_error);  // Sum overflow/limit.
  serializer->size = 8;
  channel->unregister(id);
  EXPECT_TRUE(channel->takeSnapshot());
  EXPECT_EQ(channel->payloadReallocations(), 0u);
  EXPECT_EQ(channel->poolExhausted(), 0u);
}

TEST(ChannelCapacity, TenThousandDirtySnapshotsWithControlChurnDoNotAllocate)
{
  auto channel = LogChannel::create(std::string(128, 'c'));
  CapacityWorker first;
  CapacityWorker second;
  CapacityWorker changing;
  uint64_t value = 42;
  auto id = channel->registerValue("value", &value);
  auto logged = channel->createLoggedValue<uint64_t>("changing", 42);
  channel->addDataSink(first);
  channel->addDataSink(second);
  channel->setStrictMode(true);
  ASSERT_TRUE(channel->takeSnapshot());
  first.drain();
  second.drain();
  std::atomic<bool> start{ false }, done{ false };
  std::atomic<size_t> churn{ 0 };
  std::thread control([&] {
    while(!start)
      std::this_thread::yield();
    do
    {
      logged.reset();
      logged = channel->createLoggedValue<uint64_t>("changing", 42);
      channel->addDataSink(changing);
      channel->removeDataSink(changing);
      changing.drain();
      ++churn;
    } while(!done);
  });
  start = true;
  size_t allocations = 0, deallocations = 0, successes = 0;
  size_t completed_churn = 0;
  for(int batch = 0; batch < 10; ++batch)
  {
    for(int i = 0; i < 1000; ++i)
    {
      channel->setEnabled(id, (batch * 1000 + i) % 2 == 0);
      {
        DataTamerTest::AllocCounter::Scope scope;
        successes += channel->takeSnapshot();
        allocations += scope.allocations();
        deallocations += scope.deallocations();
      }
      first.drain();
      second.drain();
    }
    if(batch != 9)
    {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      while(churn.load() <= completed_churn &&
            std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
      ASSERT_GT(churn.load(), completed_churn) << "control thread stalled";
      completed_churn = churn.load();
    }
  }
  done = true;
  control.join();
  EXPECT_GT(churn.load(), 0u);
  EXPECT_GT(successes, 0u);
  EXPECT_EQ(allocations, 0u);
  EXPECT_EQ(deallocations, 0u);
  EXPECT_EQ(channel->payloadReallocations(), 0u);
}

TEST(ChannelCapacity, AutomaticMinimumAndStrictDropRecoverAfterShrink)
{
  auto channel = LogChannel::create("capacity");
  CapacityWorker sink;
  auto value = channel->createLoggedValue<std::vector<double>>("value");
  channel->setPoolCapacity(2);
  channel->setStrictMode(true);
  channel->addDataSink(sink);
  ASSERT_TRUE(channel->takeSnapshot());
  sink.drain();
  value->set(std::vector<double>(31));  // 4-byte length + 248 bytes fits the minimum.
  EXPECT_TRUE(channel->takeSnapshot());
  sink.drain();
  value->set(std::vector<double>(32));  // 260 bytes exceeds both initial slots.
  EXPECT_FALSE(channel->takeSnapshot());
  value->set(std::vector<double>(1));
  EXPECT_TRUE(channel->takeSnapshot());
  EXPECT_EQ(channel->droppedOversize(), 1u);
  EXPECT_EQ(channel->payloadReallocations(), 0u);
}
