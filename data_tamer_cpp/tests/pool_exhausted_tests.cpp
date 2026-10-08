#include "data_tamer/channel.hpp"
#include "data_tamer/sinks/dummy_sink.hpp"

#include "test_sinks.hpp"
#include "wait_for_sleeping_thread.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

using namespace DataTamer;

// poolExhausted() reads an atomic counter: a monitoring thread gets it while another
// thread sits in a control call that holds the control mutex. The write mutex is held
// by this thread throughout, so the control calls below are stuck for as long as the
// reader is asked.
#if defined(__linux__)
TEST(PoolExhausted, IsReadWhileStartLoggingHoldsTheControlMutex)
{
  auto channel = LogChannel::create("chan");
  DataTamerTest::Attached<DummySink> sink = DataTamerTest::manual<DummySink>();
  double value = 1.0;
  channel->registerValue("value", &value);
  channel->addDataSink(sink);

  std::atomic<pid_t> tid{ 0 };
  std::atomic<bool> finished{ false };
  std::promise<uint64_t> exhausted;
  auto read = exhausted.get_future();
  std::thread starter, reader;
  bool control_call_waits = false;
  bool read_returned = false;
  {
    auto tx = channel->scopedWrite();
    starter = std::thread([&] {
      tid = static_cast<pid_t>(syscall(SYS_gettid));
      channel->startLogging();  // takes the control mutex, then waits for the writer
      finished = true;
    });
    control_call_waits = DataTamerTest::waitForSleepingThread(tid, finished);
    reader = std::thread([&] { exhausted.set_value(channel->poolExhausted()); });
    read_returned = read.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
  }
  starter.join();
  reader.join();
  ASSERT_TRUE(control_call_waits) << "startLogging() did not wait for the writer";
  EXPECT_TRUE(read_returned) << "poolExhausted() waited for the control mutex";
  EXPECT_EQ(read.get(), 0u);
}

TEST(PoolExhausted, IsReadWhileUnregisterHoldsTheControlMutex)
{
  auto channel = LogChannel::create("chan");
  DataTamerTest::Attached<DummySink> sink = DataTamerTest::manual<DummySink>();
  double kept = 1.0;
  double dropped = 2.0;
  channel->registerValue("kept", &kept);
  const auto id = channel->registerValue("dropped", &dropped);
  channel->setPoolCapacity(2);
  channel->addDataSink(sink);
  ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
  ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
  ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::pool_exhausted);  // counted once
  sink.drain();                                                        // frees the slots

  std::atomic<pid_t> snapshot_tid{ 0 };
  std::atomic<bool> snapshot_done{ false };
  std::atomic<pid_t> unregister_tid{ 0 };
  std::atomic<bool> unregister_done{ false };
  std::promise<uint64_t> exhausted;
  auto read = exhausted.get_future();
  std::thread snapshotter, unregisterer, reader;
  bool snapshot_waits = false;
  bool unregister_waits = false;
  bool read_returned = false;
  {
    auto tx = channel->scopedWrite();
    snapshotter = std::thread([&] {
      snapshot_tid = static_cast<pid_t>(syscall(SYS_gettid));
      (void)channel->takeSnapshot();  // in progress, waiting for the writer
      snapshot_done = true;
    });
    snapshot_waits = DataTamerTest::waitForSleepingThread(snapshot_tid, snapshot_done);
    unregisterer = std::thread([&] {
      unregister_tid = static_cast<pid_t>(syscall(SYS_gettid));
      channel->unregister(id);  // takes the control mutex, waits for the snapshot
      unregister_done = true;
    });
    unregister_waits =
        DataTamerTest::waitForSleepingThread(unregister_tid, unregister_done);
    reader = std::thread([&] { exhausted.set_value(channel->poolExhausted()); });
    read_returned = read.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
  }
  snapshotter.join();
  unregisterer.join();
  reader.join();
  ASSERT_TRUE(snapshot_waits) << "the snapshot did not wait for the writer";
  ASSERT_TRUE(unregister_waits) << "unregister() did not wait for the snapshot";
  EXPECT_TRUE(read_returned) << "poolExhausted() waited for the control mutex";
  EXPECT_EQ(read.get(), 1u);
}
#endif
