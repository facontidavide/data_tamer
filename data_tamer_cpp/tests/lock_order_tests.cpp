// Writers (scopedWrite(), LoggedValue guards), the snapshot path and control
// operations used together: no combination of them may deadlock.
#include "data_tamer/channel.hpp"
#include "data_tamer/sinks/dummy_sink.hpp"
#include "hang_watchdog.hpp"
#include "test_sinks.hpp"
#include "wait_for_sleeping_thread.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

using namespace DataTamer;
using DataTamerTest::expectFinishes;

#if defined(__linux__)
namespace
{
/// A thread whose blocking the test observes through procfs; joined on destruction.
class ObservedThread
{
public:
  template <typename Function>
  explicit ObservedThread(Function function)
    : thread_([this, function] {
      tid_ = static_cast<pid_t>(syscall(SYS_gettid));
      function();
      finished_ = true;
    })
  {}
  ~ObservedThread() { thread_.join(); }
  ObservedThread(const ObservedThread&) = delete;
  ObservedThread& operator=(const ObservedThread&) = delete;

  /// True once the thread sleeps (blocked), false if it finishes first.
  bool sleeps() { return DataTamerTest::waitForSleepingThread(tid_, finished_); }

private:
  std::atomic<pid_t> tid_{ 0 };
  std::atomic<bool> finished_{ false };
  std::thread thread_;
};

/// Control operations that wait for a snapshot in progress.
enum class Control
{
  Unregister,
  RemoveSink,
  DestroyLoggedValue
};

const char* name(Control control)
{
  switch(control)
  {
    case Control::Unregister:
      return "unregister()";
    case Control::RemoveSink:
      return "removeDataSink()";
    case Control::DestroyLoggedValue:
      return "LoggedValue destructor";
  }
  return "";
}

constexpr Control kControls[] = { Control::Unregister, Control::RemoveSink,
                                  Control::DestroyLoggedValue };

/// A started channel with a raw value, a LoggedValue and a sink to take away.
struct Scene
{
  Scene()
  {
    channel->registerValue("a", &a);
    id = channel->registerValue("b", &b);
    value = channel->createLoggedValue<std::vector<double>>("v");
    channel->addDataSink(sink);
    channel->startLogging();
  }

  void run(Control control)
  {
    switch(control)
    {
      case Control::Unregister:
        channel->unregister(id);
        break;
      case Control::RemoveSink:
        channel->removeDataSink(sink);
        break;
      case Control::DestroyLoggedValue:
        value.reset();
        break;
    }
  }

  double a = 1;
  double b = 2;
  std::shared_ptr<LogChannel> channel = LogChannel::create("lock_order");
  RegistrationID id;
  std::shared_ptr<LoggedValue<std::vector<double>>> value;
  DataTamerTest::Attached<DummySink> sink = DataTamerTest::manual<DummySink>();
};
}  // namespace

// A control operation inside a transaction, while takeSnapshot() waits for that
// transaction: the operation must not wait for the snapshot.
TEST(LockOrder, ControlOperationInsideATransactionWhileASnapshotWaits)
{
  for(const auto control : kControls)
  {
    SCOPED_TRACE(name(control));
    expectFinishes([control] {
      Scene scene;
      SnapshotResult result = SnapshotResult::rejected;
      std::optional<ObservedThread> snapshot;
      {
        auto tx = scene.channel->scopedWrite();
        snapshot.emplace([&] { result = scene.channel->takeSnapshot(); });
        ASSERT_TRUE(snapshot->sleeps()) << "the snapshot must wait for the transaction";
        scene.run(control);
      }
      snapshot.reset();
      // Without its sink the channel takes nothing; otherwise the snapshot happened.
      EXPECT_EQ(result, control == Control::RemoveSink ? SnapshotResult::no_sinks :
                                                         SnapshotResult::ok);
    });
  }
}

// Three threads: a writer that queries the channel inside its transaction, a
// snapshot waiting for that writer, and a control operation waiting for snapshots.
TEST(LockOrder, ConstQueryInsideATransactionWhileAControlOperationWaits)
{
  for(const auto control : kControls)
  {
    SCOPED_TRACE(name(control));
    expectFinishes([control] {
      Scene scene;
      std::optional<ObservedThread> snapshot, controller;
      {
        auto tx = scene.channel->scopedWrite();
        snapshot.emplace([&] { (void)scene.channel->takeSnapshot(); });
        ASSERT_TRUE(snapshot->sleeps()) << "the snapshot must wait for the transaction";
        controller.emplace([&] { scene.run(control); });
        (void)controller->sleeps();  // gives it time to reach its wait, if it has one
        EXPECT_FALSE(scene.channel->getSchema().fields.empty());  // takes control_mutex
      }
    });
  }
}

// startLogging() waits for the writer's transaction without holding the control
// mutex, which the writer's const query needs.
TEST(LockOrder, StartLoggingWhileAWriterQueriesTheChannel)
{
  expectFinishes([] {
    auto channel = LogChannel::create("lock_order");
    double value = 1;
    channel->registerValue("value", &value);
    auto sink = DataTamerTest::manual<DummySink>();
    channel->addDataSink(sink);
    std::optional<ObservedThread> starter;
    {
      auto tx = channel->scopedWrite();
      starter.emplace([&] { channel->startLogging(); });
      ASSERT_TRUE(starter->sleeps()) << "startLogging() must wait for the transaction";
      EXPECT_EQ(channel->getNumberOfSinks(), 1u);  // takes control_mutex
    }
    starter.reset();
    EXPECT_TRUE(channel->isLoggingStarted());
  });
}
#endif

TEST(LockOrder, StartLoggingInsideATransaction)
{
  expectFinishes([] {
    auto channel = LogChannel::create("lock_order");
    auto value = channel->createLoggedValue<std::vector<double>>("value");
    auto sink = DataTamerTest::manual<DummySink>();
    channel->addDataSink(sink);
    {
      auto tx = channel->scopedWrite();
      value->set({ 1.0, 2.0 });
      channel->startLogging();
    }
    EXPECT_TRUE(channel->isLoggingStarted());
    EXPECT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
    sink.drain();
    EXPECT_EQ(sink->snapshotsCount(channel->getSchema().hash), 1);
  });
}
