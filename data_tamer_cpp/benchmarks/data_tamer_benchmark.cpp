#include <benchmark/benchmark.h>
#include "data_tamer/data_sink.hpp"
#include "data_tamer/data_tamer.hpp"
#include "data_tamer/details/snapshot_pool.hpp"
#include "data_tamer/details/write_mutex.hpp"
#include "../examples/geometry_types.hpp"
#include "alloc_counter.hpp"
#include "null_sink.hpp"

#include <atomic>
#include <array>
#include <chrono>
#include <mutex>
#include <thread>

using namespace DataTamer;

/// Warm up, then time takeSnapshot() while counting the allocations it makes
/// on this thread; reports them as the "allocs/op" counter.
static void measureSnapshots(benchmark::State& state, LogChannel& channel)
{
  channel.takeSnapshot();  // warm-up: buffers reach their steady-state capacity
  channel.takeSnapshot();

  DataTamerTest::AllocCounter::Scope scope;  // outside the timed loop
  for(auto _ : state)
  {
    channel.takeSnapshot();
  }
  state.counters["allocs/op"] = double(scope.allocations()) / double(state.iterations());
}

static void DT_Doubles(benchmark::State& state)
{
  std::vector<double> values(size_t(state.range(0)));
  auto registry = ChannelsRegistry();
  auto channel = registry.getChannel("channel");
  channel->addDataSink(NullSink::create());
  channel->registerValue("values", &values);
  measureSnapshots(state, *channel);
}

static void DT_PoseType(benchmark::State& state)
{
  std::vector<TestTypes::Pose> poses(size_t(state.range(0)));
  auto registry = ChannelsRegistry();
  auto channel = registry.getChannel("channel");
  channel->addDataSink(NullSink::create());
  channel->registerValue("values", &poses);
  measureSnapshots(state, *channel);
}

// 1000 doubles, N sinks
static void DT_MultiSink(benchmark::State& state)
{
  std::vector<double> values(1000);
  auto registry = ChannelsRegistry();
  auto channel = registry.getChannel("channel");
  for(int i = 0; i < state.range(0); i++)
  {
    channel->addDataSink(NullSink::create());
  }
  channel->registerValue("values", &values);
  measureSnapshots(state, *channel);
}

// LoggedValue<double>::set from the calling thread
static void DT_LoggedValueSet(benchmark::State& state)
{
  auto registry = ChannelsRegistry();
  auto channel = registry.getChannel("channel");
  channel->addDataSink(NullSink::create());
  std::vector<std::shared_ptr<LoggedValue<double>>> values;
  for(int i = 0; i < state.range(0); i++)
  {
    values.push_back(channel->createLoggedValue<double>("v" + std::to_string(i)));
  }
  double x = 0;
  for(auto _ : state)
  {
    for(auto& v : values)
    {
      v->set(x);
    }
    x += 1.0;
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

// takeSnapshot on this thread while another thread hammers set() on 100 LoggedValues
static void snapshotWithWriter(benchmark::State& state, bool transactions)
{
  auto registry = ChannelsRegistry();
  auto channel = registry.getChannel("channel");
  channel->addDataSink(NullSink::create());
  std::vector<std::shared_ptr<LoggedValue<double>>> values;
  for(int i = 0; i < 100; i++)
  {
    values.push_back(channel->createLoggedValue<double>("v" + std::to_string(i)));
  }
  std::vector<double> plain(1000);
  channel->registerValue("plain", &plain);

  std::atomic_bool run{ true };
  std::thread writer([&] {
    double x = 0;
    while(run)
    {
      if(transactions)
      {
        auto transaction = channel->scopedWrite();
        for(auto& v : values)
        {
          v->set(x);
        }
      }
      else
      {
        for(auto& v : values)
        {
          v->set(x);
        }
      }
      x += 1.0;
    }
  });

  measureSnapshots(state, *channel);
  run = false;
  writer.join();
}

static void DT_SnapshotWithWriter(benchmark::State& state)
{
  snapshotWithWriter(state, false);
}

static void DT_SnapshotWithTransactionWriter(benchmark::State& state)
{
  snapshotWithWriter(state, true);
}

static void DT_WriteMutexTryLock(benchmark::State& state)
{
  WriteMutex mutex;
  for(auto _ : state)
  {
    bool acquired = mutex.try_lock();
    benchmark::DoNotOptimize(acquired);
    mutex.unlock();
  }
}

static void DT_StdMutexTryLock(benchmark::State& state)
{
  std::mutex mutex;
  for(auto _ : state)
  {
    bool acquired = mutex.try_lock();
    benchmark::DoNotOptimize(acquired);
    mutex.unlock();
  }
}

static void DT_WriteMutexSpinThenLock(benchmark::State& state)
{
  WriteMutex mutex;
  std::atomic_bool run{ true };
  std::atomic<uint64_t> requested{ 0 };
  std::atomic<uint64_t> held{ 0 };
  const auto hold = std::chrono::microseconds(state.range(0));
  std::thread writer([&] {
    uint64_t handled = 0;
    while(run.load(std::memory_order_relaxed))
    {
      const uint64_t request = requested.load(std::memory_order_acquire);
      if(request == handled)
      {
        std::this_thread::yield();
        continue;
      }
      mutex.lock();
      held.store(request, std::memory_order_release);
      const auto deadline = std::chrono::steady_clock::now() + hold;
      while(std::chrono::steady_clock::now() < deadline)
      {
      }
      mutex.unlock();
      handled = request;
    }
  });

  uint64_t blocked = 0;
  for(auto _ : state)
  {
    state.PauseTiming();
    const uint64_t request = requested.fetch_add(1, std::memory_order_release) + 1;
    while(held.load(std::memory_order_acquire) != request)
    {
      std::this_thread::yield();
    }
    state.ResumeTiming();
    blocked += mutex.lockWithSpin();
    mutex.unlock();
  }
  run.store(false, std::memory_order_relaxed);
  writer.join();
  state.counters["blocked/op"] = double(blocked) / double(state.iterations());
  state.SetLabel("writer busy-hold; time is lock acquisition after handoff");
}

static void DT_SnapshotPoolTryAcquire(benchmark::State& state)
{
  constexpr size_t kCapacity = 64;
  SnapshotPool pool(kCapacity, 0, 0);
  std::vector<PoolSlot*> busy;
  for(int64_t i = 0; i < state.range(0); ++i)
  {
    busy.push_back(pool.tryAcquire());
  }

  for(auto _ : state)
  {
    PoolSlot* slot = pool.tryAcquire();
    benchmark::DoNotOptimize(slot);
    SnapshotPool::release(slot);
  }
  for(PoolSlot* slot : busy)
  {
    SnapshotPool::release(slot);
  }
}

static void DT_SnapshotRefCloneDestroy(benchmark::State& state)
{
  auto pool = std::make_shared<SnapshotPool>(1, 0, 0);
  SnapshotRef original(pool, pool->tryAcquire());
  for(auto _ : state)
  {
    auto clone = original.clone();
    benchmark::DoNotOptimize(clone);
  }
}

static void DT_SnapshotPoolRoundTrip2Consumers(benchmark::State& state)
{
  auto pool = std::make_shared<SnapshotPool>(1, 0, 0);
  std::array<std::atomic<PoolSlot*>, 2> mailboxes{};
  std::atomic_bool run{ true };
  std::atomic<uint64_t> completed{ 0 };
  std::array<std::thread, 2> consumers;
  for(size_t i = 0; i < consumers.size(); ++i)
  {
    consumers[i] = std::thread([&, i] {
      while(run.load(std::memory_order_relaxed))
      {
        PoolSlot* slot = mailboxes[i].exchange(nullptr, std::memory_order_acquire);
        if(slot)
        {
          SnapshotPool::release(slot);
          completed.fetch_add(1, std::memory_order_release);
        }
        else
        {
          std::this_thread::yield();
        }
      }
    });
  }

  uint64_t expected = 0;
  for(auto _ : state)
  {
    PoolSlot* slot = pool->tryAcquire();
    for(auto& mailbox : mailboxes)
    {
      SnapshotPool::addRef(slot);
      mailbox.store(slot, std::memory_order_release);
    }
    SnapshotPool::release(slot);
    expected += mailboxes.size();
    while(completed.load(std::memory_order_acquire) != expected)
    {
      std::this_thread::yield();
    }
  }
  run.store(false, std::memory_order_relaxed);
  for(auto& consumer : consumers)
  {
    consumer.join();
  }
  state.SetLabel("one acquire/publish/release round-trip through two consumers");
}

BENCHMARK(DT_Doubles)->Arg(125)->Arg(250)->Arg(500)->Arg(1000)->Arg(2000);
BENCHMARK(DT_PoseType)->Arg(125)->Arg(250)->Arg(500)->Arg(1000);
BENCHMARK(DT_MultiSink)->Arg(1)->Arg(2)->Arg(4);
BENCHMARK(DT_LoggedValueSet)->Arg(10)->Arg(100)->Arg(1000);
BENCHMARK(DT_SnapshotWithWriter);
BENCHMARK(DT_SnapshotWithTransactionWriter);
BENCHMARK(DT_WriteMutexTryLock);
BENCHMARK(DT_StdMutexTryLock);
BENCHMARK(DT_WriteMutexSpinThenLock)->ArgName("hold_us")->Arg(0)->Arg(1)->Arg(5);
BENCHMARK(DT_SnapshotPoolTryAcquire)->ArgName("busy_slots")->Arg(0)->Arg(32)->Arg(63);
BENCHMARK(DT_SnapshotRefCloneDestroy);
BENCHMARK(DT_SnapshotPoolRoundTrip2Consumers);

BENCHMARK_MAIN();
