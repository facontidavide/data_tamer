#include "data_tamer/channel.hpp"
#include "data_tamer/sinks/dummy_sink.hpp"

#include "gate.hpp"
#include "observed_thread.hpp"
#include "paused_serializer.hpp"
#include "test_sinks.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <memory>
#include <thread>

using namespace DataTamer;

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
  auto serializer = std::make_shared<DataTamerTest::PausedSerializer>();
  DataTamerTest::CustomValue paused;
  double dropped = 2.0;
  channel->registerCustomValue("paused", &paused, serializer);
  const auto id = channel->registerValue("dropped", &dropped);
  channel->setPoolCapacity(2);
  channel->addDataSink(sink);
  ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
  ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
  ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::pool_exhausted);  // counted once
  sink.drain();                                                        // frees the slots

  serializer->gate = &gate;
  std::thread snapshotter([&] { (void)channel->takeSnapshot(); });
  const bool snapshot_parked = gate.waitEntered();

  // It holds the control mutex and waits for the snapshot.
  DataTamerTest::ObservedThread unregisterer([&] { channel->unregister(id); });
  const bool unregister_waits = unregisterer.sleeps();

  std::promise<uint64_t> exhausted;
  auto read = exhausted.get_future();
  std::thread reader([&] { exhausted.set_value(channel->poolExhausted()); });
  const bool read_returned =
      read.wait_for(std::chrono::seconds(2)) == std::future_status::ready;

  gate.release();
  snapshotter.join();
  reader.join();
  ASSERT_TRUE(snapshot_parked) << "the snapshot did not reach the serializer";
  ASSERT_TRUE(unregister_waits) << "unregister() did not wait for the snapshot";
  EXPECT_TRUE(read_returned) << "poolExhausted() waited for the control mutex";
  EXPECT_EQ(read.get(), 1u);
}
#endif
