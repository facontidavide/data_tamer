#include "data_tamer/data_tamer.hpp"
#include "alloc_counter.hpp"
#include "wait_for_sleeping_thread.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>
#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

using namespace DataTamer;
using DataTamerTest::AllocCounter;

namespace
{
class CheckingSink : public DataSinkBase
{
public:
  enum class Payload
  {
    PAIR,
    VECTOR
  };

  explicit CheckingSink(Payload payload) : payload_(payload) {}
  ~CheckingSink() override { stopThread(); }

  void addChannel(const std::string&, const Schema&) override {}

  bool waitFor(size_t count)
  {
    std::unique_lock lk(mutex_);
    return delivered_.wait_for(lk, std::chrono::seconds(5),
                               [&] { return delivered_count_ >= count; });
  }

  size_t delivered() const
  {
    std::scoped_lock lk(mutex_);
    return delivered_count_;
  }

  size_t errors() const
  {
    std::scoped_lock lk(mutex_);
    return errors_;
  }

protected:
  bool storeSnapshot(const Snapshot& snapshot) override
  {
    bool valid = false;
    if(payload_ == Payload::PAIR && snapshot.payload.size() == 2 * sizeof(double))
    {
      double a = 0;
      double b = 0;
      std::memcpy(&a, snapshot.payload.data(), sizeof(a));
      std::memcpy(&b, snapshot.payload.data() + sizeof(a), sizeof(b));
      valid = (a == b);
    }
    else if(payload_ == Payload::VECTOR && snapshot.payload.size() >= sizeof(uint32_t))
    {
      uint32_t size = 0;
      std::memcpy(&size, snapshot.payload.data(), sizeof(size));
      valid = snapshot.payload.size() == sizeof(size) + size_t(size) * sizeof(double);
      for(size_t i = 0; valid && i < size; ++i)
      {
        double value = 0;
        std::memcpy(&value, snapshot.payload.data() + sizeof(size) + i * sizeof(value),
                    sizeof(value));
        valid = value == double(size);
      }
    }

    {
      std::scoped_lock lk(mutex_);
      ++delivered_count_;
      errors_ += !valid;
    }
    delivered_.notify_all();
    return true;
  }

private:
  Payload payload_;
  mutable std::mutex mutex_;
  std::condition_variable delivered_;
  size_t delivered_count_ = 0;
  size_t errors_ = 0;
};

struct Pair
{
  double a = 0;
  double b = 0;
};

template <typename AddField>
std::string_view TypeDefinition(Pair& p, AddField& add)
{
  add("a", &p.a);
  add("b", &p.b);
  return "Pair";
}

struct CustomValue
{
  uint8_t value = 0;
};

class ProbeSerializer : public CustomSerializer
{
public:
  const std::string& typeName() const override { return name_; }
  size_t serializedSize(const void*) const override
  {
    ++size_calls;
    if(throw_on_size)
    {
      throw std::runtime_error("disabled value was sized");
    }
    return 1;
  }
  bool isFixedSize() const override { return true; }
  void serialize(const void* value, SerializeMe::SpanBytes& dest) const override
  {
    if(throw_on_serialize)
    {
      throw std::runtime_error("serialize failed");
    }
    dest.data()[0] = static_cast<const CustomValue*>(value)->value;
    dest.trimFront(1);
  }

  mutable size_t size_calls = 0;
  bool throw_on_size = false;
  bool throw_on_serialize = false;

private:
  std::string name_ = "CustomValue";
};

void nestTransactions(const std::vector<std::shared_ptr<LogChannel>>& channels,
                      size_t index, bool probe_mutexes)
{
  if(index == channels.size())
  {
    for(const auto& channel : channels)
    {
      ASSERT_TRUE(channel->sharedState()->inTransactionOnThisThread());
    }
    if(probe_mutexes)
    {
      std::atomic_bool all_locked{ true };
      std::thread probe([&] {
        for(const auto& channel : channels)
        {
          if(channel->writeMutex().try_lock())
          {
            all_locked = false;
            channel->writeMutex().unlock();
          }
        }
      });
      probe.join();
      ASSERT_TRUE(all_locked);
    }
    return;
  }
  auto tx = channels[index]->scopedWrite();
  nestTransactions(channels, index + 1, probe_mutexes);
}
}  // namespace

TEST(Transaction, ScopedWriteOwnsTheSharedWriteMutex)
{
  auto channel = LogChannel::create("chan");
  {
    auto tx = channel->scopedWrite();
    ASSERT_TRUE(channel->sharedState()->inTransactionOnThisThread());
    std::atomic_bool acquired{ false };
    std::thread probe([&] {
      acquired = channel->writeMutex().try_lock();
      if(acquired)
      {
        channel->writeMutex().unlock();
      }
    });
    probe.join();
    ASSERT_FALSE(acquired);
  }
  ASSERT_FALSE(channel->sharedState()->inTransactionOnThisThread());
  ASSERT_TRUE(channel->writeMutex().try_lock());
  channel->writeMutex().unlock();
}

TEST(Transaction, NestedSetAndGetDoNotRelock)
{
  auto channel = LogChannel::create("chan");
  auto vec = channel->createLoggedValue<std::vector<double>>("vec");
  auto scalar = channel->createLoggedValue<double>("scalar");
  {
    auto tx = channel->scopedWrite();
    vec->set({ 1.0, 2.0 });
    scalar->set(3.0);
    ASSERT_EQ(vec->get(), (std::vector<double>{ 1.0, 2.0 }));
  }
  ASSERT_EQ(scalar->get(), 3.0);
}

TEST(Transaction, NestingAcrossMoreThanEightChannelsDoesNotAllocateOrAlias)
{
  static_assert(!std::is_move_constructible_v<ChannelSharedState::Transaction>);
  std::vector<std::shared_ptr<LogChannel>> channels;
  for(int i = 0; i < 10; ++i)
  {
    channels.push_back(LogChannel::create("chan" + std::to_string(i)));
  }

  size_t allocation_count = 0;
  {
    AllocCounter::Scope allocations;
    nestTransactions(channels, 0, false);
    allocation_count = allocations.allocations();
  }
  ASSERT_EQ(allocation_count, 0u);
  nestTransactions(channels, 0, true);
}

// Runs `step` on a writer thread while snapshotting until the writer has run at
// least once (a busy two-core runner may not schedule it during the first 2000
// calls), then requires every accepted snapshot to have been delivered intact.
void raceWriterAgainstSnapshots(LogChannel& channel, CheckingSink& sink,
                                const std::function<void()>& step)
{
  std::atomic_bool stop{ false };
  std::atomic<size_t> iterations{ 0 };
  std::thread writer([&] {
    while(!stop.load(std::memory_order_relaxed))
    {
      step();
      iterations++;
    }
  });
  size_t accepted = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  for(int i = 0;
      i < 2000 || (iterations.load() == 0 && std::chrono::steady_clock::now() < deadline);
      ++i)
  {
    accepted += channel.takeSnapshot();
  }
  stop = true;
  writer.join();
  ASSERT_GT(iterations.load(), 0u) << "writer thread never ran";
  ASSERT_GT(accepted, 0u);
  ASSERT_TRUE(sink.waitFor(accepted));
  ASSERT_EQ(sink.delivered(), accepted);
  ASSERT_EQ(sink.errors(), 0u);
}

TEST(Transaction, ValuesWrittenTogetherAppearTogetherInDeliveredSnapshots)
{
  auto channel = LogChannel::create("chan");
  auto sink = std::make_shared<CheckingSink>(CheckingSink::Payload::PAIR);
  channel->addDataSink(sink);
  auto a = channel->createLoggedValue<double>("a", 1.0);
  auto b = channel->createLoggedValue<double>("b", 1.0);

  double value = 1.0;
  raceWriterAgainstSnapshots(*channel, *sink, [&] {
    auto tx = channel->scopedWrite();
    a->set(value);
    std::this_thread::yield();
    b->set(value);
    value = value == 1.0 ? 2.0 : 1.0;
  });
}

TEST(Transaction, RawPointerWritesAppearTogetherInDeliveredSnapshots)
{
  auto channel = LogChannel::create("chan");
  auto sink = std::make_shared<CheckingSink>(CheckingSink::Payload::PAIR);
  channel->addDataSink(sink);
  Pair pair{ 1.0, 1.0 };
  channel->registerValue("pair", &pair);

  double value = 1.0;
  raceWriterAgainstSnapshots(*channel, *sink, [&] {
    std::lock_guard<Mutex> lock(channel->writeMutex());
    pair.a = value;
    std::this_thread::yield();
    pair.b = value;
    value = value == 1.0 ? 2.0 : 1.0;
  });
}

TEST(Transaction, VectorSetAndSnapshotRaceDeliversValidPayloads)
{
  auto channel = LogChannel::create("chan");
  auto sink = std::make_shared<CheckingSink>(CheckingSink::Payload::VECTOR);
  channel->addDataSink(sink);
  auto vec = channel->createLoggedValue<std::vector<double>>("vec");

  size_t size = 1;
  raceWriterAgainstSnapshots(*channel, *sink, [&] {
    vec->set(std::vector<double>(size, double(size)));
    size = size == 64 ? 1 : size + 1;
  });
}

TEST(Transaction, ContentionCountersReportSnapshotHandoffAndRemainStableWhenUncontended)
{
  auto channel = LogChannel::create("chan");
  auto sink = std::make_shared<CheckingSink>(CheckingSink::Payload::PAIR);
  channel->addDataSink(sink);
  auto a = channel->createLoggedValue<double>("a", 1.0);
  auto b = channel->createLoggedValue<double>("b", 1.0);
  ASSERT_TRUE(channel->takeSnapshot());
  ASSERT_EQ(channel->writeLockContended(), 0u);
  ASSERT_EQ(channel->writeLockWaitMaxNs(), 0u);

  std::atomic_bool started{ false }, finished{ false };
  std::thread snapshot;
  uint64_t elapsed = 0;
  {
    auto tx = channel->scopedWrite();
    snapshot = std::thread([&] {
      started = true;
      const auto before = std::chrono::steady_clock::now();
      EXPECT_TRUE(channel->takeSnapshot());
      elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - before)
                    .count();
      finished = true;
    });
    while(!started)
      std::this_thread::yield();
    EXPECT_FALSE(finished.load());
  }
  snapshot.join();

  // Starting the thread does not prove that it exhausted the spin budget
  // before the transaction ended. Either report is legal for this handoff.
  const auto stats = channel->stats();
  EXPECT_LE(stats.write_lock_contended, 1u);
  if(stats.write_lock_contended == 0)
    EXPECT_EQ(stats.write_lock_wait_max_ns, 0u);
  EXPECT_LE(stats.write_lock_wait_max_ns, elapsed);
  EXPECT_TRUE(channel->takeSnapshot());  // an uncontended snapshot moves neither counter
  EXPECT_EQ(channel->stats().write_lock_contended, stats.write_lock_contended);
  EXPECT_EQ(channel->stats().write_lock_wait_max_ns, stats.write_lock_wait_max_ns);
}

TEST(Transaction, ObservedSleepingSnapshotAdvancesContentionCounters)
{
#if defined(__linux__)
  if(!std::ifstream("/proc/self/stat"))
    GTEST_SKIP() << "needs readable procfs";
  auto channel = LogChannel::create("chan");
  auto sink = std::make_shared<CheckingSink>(CheckingSink::Payload::PAIR);
  channel->addDataSink(sink);
  auto a = channel->createLoggedValue<double>("a", 1.0);
  auto b = channel->createLoggedValue<double>("b", 1.0);
  ASSERT_TRUE(channel->takeSnapshot());

  std::atomic<pid_t> tid{ 0 };
  std::atomic_bool finished{ false };
  std::thread snapshot;
  bool observed = false, accepted = false;
  {
    auto tx = channel->scopedWrite();
    snapshot = std::thread([&] {
      tid = static_cast<pid_t>(syscall(SYS_gettid));
      accepted = channel->takeSnapshot();
      finished = true;
    });
    observed = DataTamerTest::waitForSleepingThread(tid, finished);
  }
  snapshot.join();
  ASSERT_TRUE(observed) << "snapshot did not sleep on the held writer mutex";
  EXPECT_TRUE(accepted);
  EXPECT_EQ(channel->writeLockContended(), 1u);
  EXPECT_GT(channel->writeLockWaitMaxNs(), 0u);
  const auto stats = channel->stats();
  EXPECT_EQ(stats.write_lock_contended, 1u);
  EXPECT_EQ(stats.write_lock_wait_max_ns, channel->writeLockWaitMaxNs());
#else
  GTEST_SKIP() << "observing a blocked snapshot requires Linux procfs";
#endif
}

TEST(Transaction, LoneScalarSetDoesNotTakeWriteMutex)
{
  auto channel = LogChannel::create("chan");
  auto value = channel->createLoggedValue<double>("value");
  channel->writeMutex().lock();
  value->set(1.0);
  ASSERT_EQ(value->get(), 1.0);
  channel->writeMutex().unlock();
}

TEST(Transaction, DisabledAndDestroyedValuesAreNotSized)
{
  auto channel = LogChannel::create("chan");
  auto sink = std::make_shared<CheckingSink>(CheckingSink::Payload::VECTOR);
  channel->addDataSink(sink);
  CustomValue value;
  auto serializer = std::make_shared<ProbeSerializer>();
  const auto id = channel->registerCustomValue("value", &value, serializer);
  auto vec = channel->createLoggedValue<std::vector<double>>("vec");
  ASSERT_TRUE(channel->takeSnapshot());
  ASSERT_GT(serializer->size_calls, 0u);

  channel->unregister(id);
  serializer->size_calls = 0;
  serializer->throw_on_size = true;
  ASSERT_NO_THROW(channel->takeSnapshot());
  ASSERT_EQ(serializer->size_calls, 0u);

  vec.reset();
  ASSERT_NO_THROW(channel->takeSnapshot());
}

TEST(Transaction, SerializationExceptionReleasesWriteMutex)
{
  auto channel = LogChannel::create("chan");
  auto sink = std::make_shared<CheckingSink>(CheckingSink::Payload::VECTOR);
  channel->addDataSink(sink);
  CustomValue value;
  auto serializer = std::make_shared<ProbeSerializer>();
  serializer->throw_on_serialize = true;
  channel->registerCustomValue("value", &value, serializer);

  ASSERT_THROW(channel->takeSnapshot(), std::runtime_error);
  ASSERT_TRUE(channel->writeMutex().try_lock());
  channel->writeMutex().unlock();
}

// Values enabled inside one transaction must appear together in the snapshot
// that observes the transaction, never only the first of them.
TEST(Transaction, ValuesEnabledInsideATransactionAppearTogether)
{
#if defined(__linux__)
  if(!std::ifstream("/proc/self/stat"))
    GTEST_SKIP() << "needs readable procfs";
  auto channel = LogChannel::create("chan");
  auto sink = std::make_shared<CheckingSink>(CheckingSink::Payload::PAIR);
  channel->addDataSink(sink);
  auto a = channel->createLoggedValue<double>("a", 1.0);
  auto b = channel->createLoggedValue<double>("b", 1.0);
  ASSERT_TRUE(channel->takeSnapshot());  // freeze; a valid pair
  ASSERT_TRUE(sink->waitFor(1));
  a->setEnabled(false);
  b->setEnabled(false);

  std::atomic<pid_t> tid{ 0 };
  std::atomic<bool> finished{ false };
  bool ok = false;
  std::thread snapshotter;
  {
    auto tx = channel->scopedWrite();
    a->set(2.0);  // enables a
    snapshotter = std::thread([&] {
      tid = static_cast<pid_t>(syscall(SYS_gettid));
      ok = channel->takeSnapshot();  // blocks on the write mutex
      finished = true;
    });
    ASSERT_TRUE(DataTamerTest::waitForSleepingThread(tid, finished));
    b->set(2.0);  // enables b, still inside the transaction
  }
  snapshotter.join();
  ASSERT_TRUE(ok);
  ASSERT_TRUE(sink->waitFor(2));
  EXPECT_EQ(sink->errors(), 0u);  // PAIR payload: both or neither, never one value
#else
  GTEST_SKIP() << "observing a blocked snapshot requires Linux procfs";
#endif
}
