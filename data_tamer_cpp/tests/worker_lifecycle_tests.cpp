// SinkWorker lifecycle calls (stop(), start(), drain(), destruction) made from sink
// callbacks and from several threads at once.
#include "data_tamer/channel.hpp"
#include "data_tamer/data_sink.hpp"
#include "data_tamer/data_tamer.hpp"
#include "gate.hpp"
#include "hang_watchdog.hpp"
#include "test_sinks.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <latch>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace DataTamer;
using DataTamerTest::Attached;
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

/// Counts its onStop() and onStart() calls.
class CountingSink : public DataSink
{
public:
  std::atomic<int> stops{ 0 };
  std::atomic<int> starts{ 0 };

protected:
  void onSchema(const Schema&) override {}
  void onSnapshot(const SnapshotRef&) override {}
  void onStop() override { ++stops; }
  void onStart() override { ++starts; }
};

/// What a QueryingSink saw; outlives the sink.
struct QueryJournal
{
  DataTamerTest::Gate gate;  // parks the callback while it holds the channel
  std::atomic<int> stops{ 0 };
  std::atomic<bool> destroyed{ false };
};

/// Queries its channel from onSnapshot() through a weak_ptr (a const query, allowed
/// from a callback), and waits there until the test has dropped its own reference.
class QueryingSink : public DataSink
{
public:
  QueryingSink(std::weak_ptr<LogChannel> channel, std::shared_ptr<QueryJournal> journal)
    : channel_(std::move(channel)), journal_(std::move(journal))
  {}
  ~QueryingSink() override { journal_->destroyed = true; }

protected:
  void onSchema(const Schema&) override {}
  void onSnapshot(const SnapshotRef&) override
  {
    if(auto channel = channel_.lock())
    {
      (void)channel->getSchema();
      journal_->gate.pause();
    }  // the last reference to the channel, and so to this worker, can end here
  }
  void onStop() override { ++journal_->stops; }

private:
  std::weak_ptr<LogChannel> channel_;
  std::shared_ptr<QueryJournal> journal_;
};

/// Runs `function` on two threads released together; returns how many calls threw.
int runTwiceAtOnce(const std::function<void()>& function)
{
  std::latch ready(2);
  std::atomic<int> thrown{ 0 };
  const auto run = [&] {
    ready.arrive_and_wait();
    try
    {
      function();
    }
    catch(...)
    {
      ++thrown;
    }
  };
  std::thread first(run);
  std::thread second(run);
  first.join();
  second.join();
  return thrown;
}

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
        Attached<ReentrantSink> sink(from, call.function);
        sink->self = sink.worker.get();
        auto channel = LogChannel::create("reentrant");
        double value = 1;
        channel->registerValue("value", &value);
        channel->startLogging();
        channel->addDataSink(sink);  // onSchema() on this thread
        ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
        ASSERT_TRUE(waitFor([&] { return sink->snapshots == 1; }));
        EXPECT_EQ(sink->outcome, Outcome::LogicError);
        // The worker still accepts and delivers, and stop() still finishes the sink.
        EXPECT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
        EXPECT_TRUE(waitFor([&] { return sink->snapshots == 2; }));
        sink.worker->stop();
        EXPECT_EQ(sink->stops, 1);
      });
    }
  }
}

// Two shutdown paths stopping the same workers at once, through stopAll() or
// stop(), or a stop() racing a start(): every call returns and the sink sees
// matching onStop() and onStart() calls.
TEST(WorkerLifecycle, ConcurrentLifecycleCallsOnOneWorker)
{
  expectFinishes([] {
    for(int round = 0; round < 100; ++round)
    {
      ChannelsRegistry registry;
      std::vector<Attached<CountingSink>> sinks;
      for(int i = 0; i < 4; ++i)
      {
        sinks.emplace_back();
        registry.addDefaultSink(sinks.back());
      }
      auto channel = registry.getChannel("stops");
      double value = 1;
      channel->registerValue("value", &value);
      ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
      ASSERT_EQ(runTwiceAtOnce([&] { registry.stopAll(); }), 0);
      for(const auto& sink : sinks)
      {
        ASSERT_EQ(sink->stops, 1);
      }
    }
    Attached<CountingSink> sink;
    for(int round = 0; round < 100; ++round)
    {
      const int stops = sink->stops;
      ASSERT_EQ(runTwiceAtOnce([&] { sink.worker->stop(); }), 0);
      ASSERT_EQ(sink->stops, stops + 1);  // the second stop() found it stopped
      std::atomic<bool> stopping{ false };
      ASSERT_EQ(runTwiceAtOnce([&] {
                  stopping.exchange(true) ? sink.worker->stop() : sink.worker->start();
                }),
                0);
      // Stopped, so the start() finishes first or the stop() does nothing.
      ASSERT_GE(sink->stops - sink->starts, 0);
      ASSERT_LE(sink->stops - sink->starts, 1);
      sink.worker->start();
      ASSERT_EQ(sink->stops, sink->starts);
    }
  });
}

// The last reference to a worker released by its own callback: the channel, which
// owned the worker, is destroyed on the worker thread. The worker still finishes:
// onStop() runs once and the sink is destroyed.
TEST(WorkerLifecycle, WorkerReleasedByItsOwnCallbackFinishesTheSink)
{
  expectFinishes([] {
    auto journal = std::make_shared<QueryJournal>();
    auto channel = LogChannel::create("released_by_callback");
    double value = 1;
    channel->registerValue("value", &value);
    channel->addDataSink(  // the channel holds the only reference to the worker
        SinkWorker::create<QueryingSink>(channel, journal));
    ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
    ASSERT_TRUE(journal->gate.waitEntered());
    channel.reset();
    journal->gate.release();
    EXPECT_TRUE(waitFor([&] { return journal->destroyed.load(); }));
    EXPECT_EQ(journal->stops, 1);
  });
}
