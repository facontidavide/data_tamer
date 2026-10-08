// SinkWorker's typed access and real-time getters.
#include "data_tamer/data_sink.hpp"
#include "data_tamer/sinks/dummy_sink.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <typeinfo>

using namespace DataTamer;

namespace
{
class OtherSink : public DataSink
{
protected:
  void onSchema(const Schema&) override {}
  void onSnapshot(const SnapshotRef&) override {}
};
}  // namespace

// A const worker still gives typed, read-only access to its sink, and the counters
// a real-time thread reads are noexcept.
TEST(SinkWorkerApi, ConstTypedAccessAndNoexceptCounters)
{
  const auto worker = std::make_shared<SinkWorker>(std::make_unique<DummySink>(),
                                                   SinkWorker::Delivery::Manual);
  const SinkWorker& view = *worker;
  EXPECT_EQ(view.as<DummySink>().schemasCount(), 0u);
  EXPECT_THROW((void)view.as<OtherSink>(), std::bad_cast);
  static_assert(noexcept(view.delivered()));
  static_assert(noexcept(view.errors()));
  static_assert(noexcept(view.queueHighWater()));
  EXPECT_EQ(view.delivered(), 0u);
}
