// Several threads taking snapshots of one channel.
#include "data_tamer/channel.hpp"
#include "data_tamer/data_sink.hpp"
#include "test_sinks.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace DataTamer;

namespace
{
constexpr size_t kProducers = 4;

/// Counts deliveries and checks that each producer's counter never goes back.
class OrderSink : public DataSink
{
public:
  std::atomic<uint64_t> delivered{ 0 };
  std::atomic<uint64_t> errors{ 0 };  // malformed payloads and counters going back

protected:
  void onSchema(const Schema&) override {}
  void onSnapshot(const SnapshotRef& snapshot) override
  {
    ++delivered;
    std::array<uint64_t, kProducers> values{};
    if(snapshot->payload.size() != sizeof(values))
    {
      ++errors;
      return;
    }
    std::memcpy(values.data(), snapshot->payload.data(), sizeof(values));
    for(size_t i = 0; i < kProducers; ++i)
    {
      errors += values[i] < last_[i];
    }
    last_ = values;
  }

private:
  std::array<uint64_t, kProducers> last_{};
};
}  // namespace

// Producers are serialized from the slot to the last push, so every sink receives
// every accepted snapshot once, in the order the snapshots were serialized: each
// counter, read under the write mutex, never decreases. Meant for ThreadSanitizer.
TEST(MultiProducer, SeveralThreadsTakeSnapshotsOfOneChannel)
{
  constexpr int kSnapshotsPerThread = 20000;
  auto channel = LogChannel::create("multi_producer");
  std::array<std::atomic<uint64_t>, kProducers> counters{};
  for(size_t i = 0; i < kProducers; ++i)
  {
    channel->registerValue("counter_" + std::to_string(i), &counters[i]);
  }
  DataTamerTest::Attached<OrderSink> first, second;
  channel->addDataSink(first);
  channel->addDataSink(second);
  channel->startLogging();

  std::array<uint64_t, kProducers> accepted{};
  std::vector<std::thread> producers;
  for(size_t i = 0; i < kProducers; ++i)
  {
    producers.emplace_back([&, i] {
      for(int n = 0; n < kSnapshotsPerThread; ++n)
      {
        counters[i].fetch_add(1, std::memory_order_relaxed);
        const auto result = n % 2 ? channel->takeSnapshot() : channel->tryTakeSnapshot();
        accepted[i] += result == SnapshotResult::ok;
      }
    });
  }
  for(auto& producer : producers)
  {
    producer.join();
  }
  first.worker->stop();  // delivers everything queued
  second.worker->stop();
  uint64_t total = 0;
  for(const auto count : accepted)
  {
    total += count;
  }
  EXPECT_GT(total, 0u);
  EXPECT_EQ(first->delivered, total);
  EXPECT_EQ(second->delivered, total);
  EXPECT_EQ(first->errors, 0u);
  EXPECT_EQ(second->errors, 0u);
}
