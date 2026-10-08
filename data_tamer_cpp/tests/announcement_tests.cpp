// How the schema reaches sinks that attach while startLogging() runs, or while
// another thread attaches the same sink.
#include "data_tamer/channel.hpp"
#include "data_tamer/data_sink.hpp"
#include "observed_thread.hpp"
#include "test_sinks.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <vector>

using namespace DataTamer;

namespace
{
/// Parks a callback until the test releases it.
struct Gate
{
  void pause()
  {
    std::unique_lock lock(mutex);
    entered = true;
    cv.notify_all();
    cv.wait(lock, [&] { return released; });
  }
  bool waitEntered()
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

  std::mutex mutex;
  std::condition_variable cv;
  bool entered = false;
  bool released = false;
};

/// Records the schemas it heard and the snapshots whose schema it never heard.
class AnnouncedSink : public DataSink
{
public:
  Gate* gate = nullptr;               // parks the first onSchema()
  std::atomic<bool> reject{ false };  // onSchema() throws while set

  std::vector<uint64_t> schemas() const
  {
    std::lock_guard lock(mutex_);
    return schemas_;
  }
  int snapshots() const
  {
    std::lock_guard lock(mutex_);
    return snapshots_;
  }
  int unknownSnapshots() const
  {
    std::lock_guard lock(mutex_);
    return unknown_;
  }

protected:
  void onSchema(const Schema& schema) override
  {
    if(gate && !gated_)
    {
      gated_ = true;
      gate->pause();
    }
    if(reject)
    {
      throw std::runtime_error("schema refused");
    }
    std::lock_guard lock(mutex_);
    schemas_.push_back(schema.hash);
  }
  void onSnapshot(const SnapshotRef& snapshot) override
  {
    std::lock_guard lock(mutex_);
    ++snapshots_;
    if(std::find(schemas_.begin(), schemas_.end(), snapshot->schema_hash) ==
       schemas_.end())
    {
      ++unknown_;
    }
  }

private:
  bool gated_ = false;  // callbacks are serialized: no lock needed
  mutable std::mutex mutex_;
  std::vector<uint64_t> schemas_;
  int snapshots_ = 0;
  int unknown_ = 0;
};
}  // namespace

// addDataSink() hears the frozen schema, then startLogging() fails, the schema
// reopens and a registration changes it: the late sink must still hear the final
// schema before its first snapshot, whether logging starts again before or after
// the late announcement returns.
TEST(Announcement, SinkAttachedDuringAFailedStartLoggingHearsTheFinalSchema)
{
  for(const bool restart_during_announcement : { false, true })
  {
    SCOPED_TRACE(restart_during_announcement ? "restarted during the announcement" :
                                               "restarted after the announcement");
    auto channel = LogChannel::create("announcement");
    double a = 1, b = 2;
    channel->registerValue("a", &a);
    Gate failing_gate, late_gate;
    auto failing = DataTamerTest::manual<AnnouncedSink>();
    failing->gate = &failing_gate;
    failing->reject = true;
    channel->addDataSink(failing);
    auto late = DataTamerTest::manual<AnnouncedSink>();
    late->gate = &late_gate;

    auto start = std::async(std::launch::async, [&] { channel->startLogging(); });
    ASSERT_TRUE(failing_gate.waitEntered());  // announcing, control mutex released
    auto attach =
        std::async(std::launch::async, [&] { return channel->addDataSink(late); });
    ASSERT_TRUE(late_gate.waitEntered());  // hearing the frozen schema
    failing_gate.release();
    EXPECT_THROW(start.get(), std::runtime_error);  // the schema reopens
    channel->registerValue("b", &b);                // and changes
    failing->reject = false;
    if(restart_during_announcement)
    {
      channel->startLogging();
    }
    late_gate.release();
    EXPECT_TRUE(attach.get());
    channel->startLogging();
    ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
    late.drain();
    ASSERT_FALSE(late->schemas().empty());
    EXPECT_EQ(late->schemas().back(), channel->getSchema().hash);
    EXPECT_EQ(late->snapshots(), 1);
    EXPECT_EQ(late->unknownSnapshots(), 0);
  }
}

#if defined(__linux__)
// Two threads attach the same worker to a started channel at once: its sink hears
// the schema once, one call attaches it and the other returns false.
TEST(Announcement, ConcurrentAttachmentsOfOneWorkerAnnounceOnce)
{
  auto channel = LogChannel::create("announcement");
  double a = 1;
  channel->registerValue("a", &a);
  channel->startLogging();
  Gate gate;
  auto sink = DataTamerTest::manual<AnnouncedSink>();
  sink->gate = &gate;
  std::atomic<int> attached{ 0 };
  std::optional<DataTamerTest::ObservedThread> first, second;
  first.emplace([&] { attached += channel->addDataSink(sink); });
  ASSERT_TRUE(gate.waitEntered());  // the first announcement is under way
  second.emplace([&] { attached += channel->addDataSink(sink); });
  EXPECT_TRUE(second->sleeps());  // it waits for the first announcement
  gate.release();
  first.reset();
  second.reset();
  EXPECT_EQ(attached, 1);
  EXPECT_EQ(sink->schemas().size(), 1u);
  EXPECT_EQ(channel->getNumberOfSinks(), 1u);
  ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
  sink.drain();
  EXPECT_EQ(sink->snapshots(), 1);
}
#endif
