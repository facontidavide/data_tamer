// SinkWorker lifecycle calls (stop(), start(), drain(), destruction) made from sink
// callbacks and from several threads at once.
#include "data_tamer/channel.hpp"
#include "data_tamer/data_sink.hpp"
#include "hang_watchdog.hpp"
#include "test_sinks.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

using namespace DataTamer;
using DataTamerTest::expectFinishes;

namespace
{
/// Where a lifecycle call made from a callback ended.
enum class Outcome
{
  NotCalled,
  LogicError,
  OtherException,
  Returned
};

/// Calls `call` on its own worker from the first onSchema() or onSnapshot().
class ReentrantSink : public DataSink
{
public:
  enum class From
  {
    Schema,
    Snapshot
  };

  ReentrantSink(From from, std::function<void(SinkWorker&)> call)
    : from_(from), call_(std::move(call))
  {}

  SinkWorker* self = nullptr;
  std::atomic<Outcome> outcome{ Outcome::NotCalled };
  std::atomic<int> snapshots{ 0 };
  std::atomic<int> stops{ 0 };

protected:
  void onSchema(const Schema&) override
  {
    if(from_ == From::Schema)
    {
      callOnce();
    }
  }
  void onSnapshot(const SnapshotRef&) override
  {
    if(from_ == From::Snapshot)
    {
      callOnce();
    }
    ++snapshots;
  }
  void onStop() override { ++stops; }

private:
  void callOnce()
  {
    if(outcome != Outcome::NotCalled)
    {
      return;
    }
    try
    {
      call_(*self);
      outcome = Outcome::Returned;
    }
    catch(const std::logic_error&)
    {
      outcome = Outcome::LogicError;
    }
    catch(...)
    {
      outcome = Outcome::OtherException;
    }
  }

  From from_;
  std::function<void(SinkWorker&)> call_;
};

struct Call
{
  const char* name;
  void (*function)(SinkWorker&);
};

constexpr Call kCalls[] = { { "drain()", [](SinkWorker& w) { w.drain(); } },
                            { "start()", [](SinkWorker& w) { w.start(); } },
                            { "stop()", [](SinkWorker& w) { w.stop(); } } };

bool waitFor(const std::function<bool()>& condition)
{
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while(!condition())
  {
    if(std::chrono::steady_clock::now() > deadline)
    {
      return false;
    }
    std::this_thread::yield();
  }
  return true;
}
}  // namespace

// From a callback, drain(), start() and stop() of the sink's own worker would wait
// for that callback: they throw std::logic_error and change nothing.
TEST(WorkerLifecycle, LifecycleCallsFromOwnCallbacksThrowLogicError)
{
  for(const auto from : { ReentrantSink::From::Schema, ReentrantSink::From::Snapshot })
  {
    for(const auto& call : kCalls)
    {
      SCOPED_TRACE(std::string(call.name) + " from " +
                   (from == ReentrantSink::From::Schema ? "onSchema()" : "onSnapshot()"));
      expectFinishes([from, call] {
        auto owned = std::make_unique<ReentrantSink>(from, call.function);
        auto* sink = owned.get();
        auto worker = std::make_shared<SinkWorker>(std::move(owned));
        sink->self = worker.get();
        auto channel = LogChannel::create("reentrant");
        double value = 1;
        channel->registerValue("value", &value);
        channel->startLogging();
        channel->addDataSink(worker);  // onSchema() on this thread
        ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
        ASSERT_TRUE(waitFor([&] { return sink->snapshots == 1; }));
        EXPECT_EQ(sink->outcome, Outcome::LogicError);
        // The worker still accepts and delivers, and stop() still finishes the sink.
        EXPECT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
        EXPECT_TRUE(waitFor([&] { return sink->snapshots == 2; }));
        worker->stop();
        EXPECT_EQ(sink->stops, 1);
      });
    }
  }
}
