#include "data_tamer/channel.hpp"
#include "data_tamer/sinks/dummy_sink.hpp"

#include "gate.hpp"
#include "test_sinks.hpp"
#include "wait_for_sleeping_thread.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <memory>
#include <thread>

using namespace DataTamer;

namespace
{
struct PausedValue
{
  uint64_t value = 0;
};

// Parks the next snapshot in serializedSize(), inside the snapshot, until the gate opens.
class PausingSerializer : public CustomSerializer
{
public:
  explicit PausingSerializer(DataTamerTest::Gate& gate) : gate_(gate) {}
  const std::string& typeName() const override
  {
    static const std::string name = "PausedValue";
    return name;
  }
  bool isFixedSize() const override { return true; }
  size_t serializedSize(const void*) const override
  {
    if(pause_next_.exchange(false))
    {
      gate_.pause();
    }
    return sizeof(uint64_t);
  }
  void serialize(const void* source, SerializeMe::SpanBytes& bytes) const override
  {
    std::memcpy(bytes.data(), source, sizeof(uint64_t));
    bytes.trimFront(sizeof(uint64_t));
  }
  void pauseNext() { pause_next_ = true; }

private:
  DataTamerTest::Gate& gate_;
  mutable std::atomic<bool> pause_next_{ false };
};
}  // namespace

TEST(PoolExhausted, IsZeroBeforeLoggingStarts)
{
  auto channel = LogChannel::create("chan");
  double value = 1.0;
  channel->registerValue("value", &value);
  EXPECT_EQ(channel->poolExhausted(), 0u);
}

// poolExhausted() reads an atomic counter: a monitoring thread gets it while unregister()
// holds the control mutex, waiting for a snapshot parked inside a serializer.
#if defined(__linux__)
TEST(PoolExhausted, IsReadWhileUnregisterHoldsTheControlMutex)
{
  auto channel = LogChannel::create("chan");
  DataTamerTest::Attached<DummySink> sink = DataTamerTest::manual<DummySink>();
  DataTamerTest::Gate gate;
  auto serializer = std::make_shared<PausingSerializer>(gate);
  PausedValue paused;
  double dropped = 2.0;
  channel->registerCustomValue("paused", &paused, serializer);
  const auto id = channel->registerValue("dropped", &dropped);
  channel->setPoolCapacity(2);
  channel->addDataSink(sink);
  ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
  ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
  ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::pool_exhausted);  // counted once
  sink.drain();                                                        // frees the slots

  serializer->pauseNext();
  std::thread snapshotter([&] { (void)channel->takeSnapshot(); });
  const bool snapshot_parked = gate.waitEntered();

  std::atomic<pid_t> unregister_tid{ 0 };
  std::atomic<bool> unregister_done{ false };
  std::thread unregisterer([&] {
    unregister_tid = static_cast<pid_t>(syscall(SYS_gettid));
    channel->unregister(id);  // holds the control mutex, waits for the snapshot
    unregister_done = true;
  });
  const bool unregister_waits =
      DataTamerTest::waitForSleepingThread(unregister_tid, unregister_done);

  std::promise<uint64_t> exhausted;
  auto read = exhausted.get_future();
  std::thread reader([&] { exhausted.set_value(channel->poolExhausted()); });
  const bool read_returned =
      read.wait_for(std::chrono::seconds(2)) == std::future_status::ready;

  gate.release();
  snapshotter.join();
  unregisterer.join();
  reader.join();
  ASSERT_TRUE(snapshot_parked) << "the snapshot did not reach the serializer";
  ASSERT_TRUE(unregister_waits) << "unregister() did not wait for the snapshot";
  EXPECT_TRUE(read_returned) << "poolExhausted() waited for the control mutex";
  EXPECT_EQ(read.get(), 1u);
}
#endif
