// The library's threads carry a name, for top -H, perf and debuggers (Linux only).
#include "data_tamer_v2/channel.hpp"
#include "data_tamer_v2/sinks/mcap_ring_sink.hpp"
#include "mcap_test_utils.hpp"
#include "test_sinks.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <string>

#if defined(__linux__)
#include <pthread.h>

using namespace DataTamer2;

namespace
{
std::string currentThreadName()
{
  std::array<char, 16> name{};
  pthread_getname_np(pthread_self(), name.data(), name.size());
  return name.data();
}

/// Reports the name of the thread that delivers its first snapshot. drain() would
/// deliver on the calling thread, so the test waits for this instead.
class ThreadNameSink : public DataSink
{
public:
  std::promise<std::string> name;

protected:
  void onSchema(const Schema&) override {}
  void onSnapshot(const SnapshotRef&) override
  {
    if(!reported_)
    {
      reported_ = true;
      name.set_value(currentThreadName());
    }
  }

private:
  bool reported_ = false;
};
}  // namespace

TEST(ThreadName, SinkWorkerThread)
{
  auto sink = DataTamerTest::attach<ThreadNameSink>(SinkWorker::Delivery::Threaded);
  uint64_t value = 1;
  auto channel = DataTamerTest::channelWith(sink, &value);
  auto name = sink->name.get_future();
  ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
  ASSERT_EQ(name.wait_for(std::chrono::seconds(10)), std::future_status::ready);
  EXPECT_EQ(name.get(), "dt_sink_worker");
}

TEST(ThreadName, MCAPRingSinkWriterThread)
{
  DataTamerTest::ScratchDir dir("thread_name_ring");
  auto sink = DataTamerTest::manual<MCAPRingSink>(
      DataTamerTest::ringOptions(dir.file("r.mcap"), 100));
  std::string name;
  sink->setDumpCallback([&](const MCAPRingDump&) { name = currentThreadName(); });
  DataTamerTest::Source source("thread_name", sink);
  source.take(0);
  sink.drain();
  ASSERT_TRUE(sink->requestDump());
  ASSERT_TRUE(sink->flushPendingDump());  // waits for the callback
  EXPECT_EQ(name, "dt_ring_writer");
}
#endif
