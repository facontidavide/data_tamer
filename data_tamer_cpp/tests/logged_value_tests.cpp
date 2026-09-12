#include "data_tamer/data_tamer.hpp"
#include "data_tamer/sinks/dummy_sink.hpp"
#include "alloc_counter.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <thread>
#include <type_traits>
#include <vector>

using namespace DataTamer;
using DataTamerTest::AllocCounter;

TEST(LoggedValue, IsNotMovableOrCopyable)
{
  static_assert(!std::is_copy_constructible_v<LoggedValue<double>>);
  static_assert(!std::is_copy_assignable_v<LoggedValue<double>>);
  static_assert(!std::is_move_constructible_v<LoggedValue<double>>, "moving a "
                                                                    "LoggedValue would "
                                                                    "leave the channel "
                                                                    "with a dangling "
                                                                    "pointer");
  static_assert(!std::is_move_assignable_v<LoggedValue<double>>);
}

TEST(LoggedValue, SharedPtrHandleStillWorks)
{
  auto channel = LogChannel::create("chan");
  auto v = channel->createLoggedValue<float>("f", 1.0f);
  std::shared_ptr<LoggedValue<float>> moved = std::move(v);
  ASSERT_FALSE(v);
  ASSERT_EQ(moved->get(), 1.0f);
}

TEST(LoggedValue, ScalarTraitSelectsAtomics)
{
  static_assert(is_atomic_scalar_v<double>);
  static_assert(is_atomic_scalar_v<int8_t>);
  static_assert(is_atomic_scalar_v<bool>);
  enum Color : uint8_t
  {
    RED
  };
  static_assert(is_atomic_scalar_v<Color>);
  static_assert(!is_atomic_scalar_v<std::vector<double>>);
  struct Big
  {
    double a, b;
  };
  static_assert(!is_atomic_scalar_v<Big>);
}

TEST(LoggedValue, ScalarSetIsSeenBySnapshotAndDoesNotAllocate)
{
  auto channel = LogChannel::create("chan");
  auto sink = std::make_shared<DummySink>();
  channel->addDataSink(sink);
  auto v = channel->createLoggedValue<double>("v", 1.0);
  channel->takeSnapshot();
  sink->flush();

  {
    AllocCounter::Scope scope;
    for(int i = 0; i < 1000; i++)
    {
      v->set(double(i));
    }
    ASSERT_EQ(scope.allocations(), 0u);
  }
  ASSERT_EQ(v->get(), 999.0);

  channel->takeSnapshot();
  sink->flush();
  const auto snap = sink->latestSnapshot();
  ASSERT_EQ(snap.payload.size(), sizeof(double));
  double stored = 0;
  std::memcpy(&stored, snap.payload.data(), sizeof(double));
  ASSERT_EQ(stored, 999.0);
}

TEST(LoggedValue, ScalarProxiesWriteBackAndHoldCopies)
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  auto channel = LogChannel::create("chan");
  auto v = channel->createLoggedValue<float>("f", 3.0f);
  {
    auto p = v->getMutablePtr();
    ASSERT_TRUE(p);
    *p += 1.0f;
    ASSERT_EQ(v->get(), 3.0f);  // write-back happens on destruction
  }
  ASSERT_EQ(v->get(), 4.0f);
  auto c = v->getConstPtr();
  v->set(5.0f);
  ASSERT_EQ(*c, 4.0f);  // copy taken at construction
#pragma GCC diagnostic pop
}

TEST(LoggedValue, SetEnabledFromWriterThreadNeedsNoChannel)
{
  auto channel = LogChannel::create("chan");
  auto v = channel->createLoggedValue<int32_t>("i", 1);
  channel.reset();  // channel gone; the LoggedValue must still be usable
  v->set(2);
  v->setEnabled(false);
  ASSERT_FALSE(v->isEnabled());
  v->set(3, /*auto_enable=*/true);
  ASSERT_TRUE(v->isEnabled());
  ASSERT_EQ(v->get(), 3);
}

TEST(LoggedValue, AutoEnableOnSetDirtiesMask)
{
  auto channel = LogChannel::create("chan");
  auto sink = std::make_shared<DummySink>();
  channel->addDataSink(sink);
  auto v = channel->createLoggedValue<double>("v", 1.0);
  v->setEnabled(false);
  channel->takeSnapshot();
  sink->flush();
  ASSERT_EQ(sink->latestPayloadSize(), 0u);

  v->set(2.0);  // auto_enable defaults to true
  channel->takeSnapshot();
  sink->flush();
  ASSERT_EQ(sink->latestPayloadSize(), sizeof(double));
}

// The race that existed before this plan: a writer thread hammering set()
// while the snapshot thread serializes. Must be clean under TSAN, and every
// snapshot must decode to a value the writer actually wrote.
namespace
{
// Every delivered 8-byte payload must have all bytes equal (see the writer below).
class TearingSink : public DataSinkBase
{
public:
  ~TearingSink() override { stopThread(); }
  using DataSinkBase::processQueuedSnapshots;
  std::atomic<size_t> checked{ 0 };
  std::atomic<size_t> torn{ 0 };
  void addChannel(const std::string&, const Schema&) override {}
  bool storeSnapshot(const Snapshot& snapshot) override
  {
    uint64_t seen = 0;
    if(snapshot.payload.size() != sizeof(seen))
    {
      torn++;
      return false;
    }
    std::memcpy(&seen, snapshot.payload.data(), sizeof(seen));
    for(int b = 1; b < 8; b++)
      if(((seen >> (8 * b)) & 0xFF) != (seen & 0xFF))
      {
        torn++;
        return false;
      }
    checked++;
    return true;
  }
};
}  // namespace

TEST(LoggedValue, ScalarWriterRacesSnapshotCleanly)
{
  auto channel = LogChannel::create("chan");
  auto sink = std::make_shared<TearingSink>();
  channel->addDataSink(sink);
  auto v = channel->createLoggedValue<uint64_t>("v", 0);
  channel->takeSnapshot();

  std::atomic_bool stop{ false };
  std::thread writer([&] {
    uint64_t x = 0;
    while(!stop)
    {
      // Mask to a single byte before broadcasting: 0x0101..01 * x only
      // replicates that byte across all 8 bytes while x < 256 (beyond that
      // the multiplication carries between byte positions and legitimately
      // produces non-uniform bytes, unrelated to any tearing). Masking keeps
      // "every byte identical" true indefinitely, so any deviation seen by
      // the snapshot thread can only come from a torn store/load.
      const uint64_t byte_val = (++x) & 0xFFu;
      v->set(byte_val * 0x0101010101010101ULL);
    }
  });
  for(int i = 0; i < 2000; i++)
  {
    channel->takeSnapshot();
  }
  stop = true;
  writer.join();
  sink->processQueuedSnapshots();
  ASSERT_EQ(sink->torn.load(), 0u);
  ASSERT_GT(sink->checked.load(), 0u);  // a full queue may have dropped some
}

TEST(LoggedValue, NonScalarStillWorksThroughMutablePtr)
{
  auto channel = LogChannel::create("chan");
  auto sink = std::make_shared<DummySink>();
  channel->addDataSink(sink);
  auto v = channel->createLoggedValue<std::vector<double>>("vec", { 1.0, 2.0 });
  {
    auto p = v->getMutablePtr();
    p->push_back(3.0);
    bool lockable = true;
    std::thread([&] { lockable = channel->writeMutex().try_lock(); }).join();
    ASSERT_FALSE(lockable);  // held by p
  }
  ASSERT_EQ(v->get().size(), 3u);
  channel->takeSnapshot();
  sink->flush();
  ASSERT_EQ(sink->latestPayloadSize(), sizeof(uint32_t) + 3 * sizeof(double));
}

// A writer thread growing a vector through getMutablePtr() while snapshots run.
// Without the snapshot thread taking the shared write mutex this is a
// use-after-free of the vector's buffer (TSAN/ASAN report).
TEST(LoggedValue, NonScalarProxyExcludesSnapshot)
{
  auto channel = LogChannel::create("chan");
  auto sink = std::make_shared<DummySink>();
  channel->addDataSink(sink);
  auto v = channel->createLoggedValue<std::vector<double>>("vec");
  channel->takeSnapshot();
  std::atomic_bool stop{ false };
  std::thread writer([&] {
    while(!stop)
    {
      auto p = v->getMutablePtr();
      p->assign(size_t(1 + (p->size() % 64)),
                1.0);  // size cycles 1..64, reallocating often
    }
  });
  for(int i = 0; i < 3000; i++)
  {
    channel->takeSnapshot();
  }
  stop = true;
  writer.join();
}
