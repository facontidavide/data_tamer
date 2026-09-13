#include "data_tamer/details/snapshot_pool.hpp"
#include "alloc_counter.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

using namespace DataTamer;
using DataTamerTest::AllocCounter;

static std::shared_ptr<SnapshotPool> makePool(size_t capacity = 4)
{
  return std::make_shared<SnapshotPool>(capacity, /*payload_capacity=*/256,
                                        /*mask_bytes=*/2);
}

TEST(SnapshotPool, SlotsArePreallocated)
{
  auto pool = makePool(3);
  ASSERT_EQ(pool->capacity(), 3u);
  ASSERT_EQ(pool->inUse(), 0u);
  PoolSlot* s = pool->tryAcquire();
  ASSERT_NE(s, nullptr);
  ASSERT_GE(s->snapshot.payload.capacity(), 256u);
  ASSERT_EQ(s->snapshot.active_mask.size(), 2u);
  ASSERT_EQ(s->refs.load(), 1u);
  SnapshotPool::release(s);
  ASSERT_EQ(pool->inUse(), 0u);
}

TEST(SnapshotPool, AcquireExhaustsAndCounts)
{
  auto pool = makePool(2);
  PoolSlot* a = pool->tryAcquire();
  PoolSlot* b = pool->tryAcquire();
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  ASSERT_NE(a, b);
  ASSERT_EQ(pool->tryAcquire(), nullptr);
  ASSERT_EQ(pool->exhausted(), 1u);
  SnapshotPool::release(a);
  ASSERT_EQ(pool->tryAcquire(), a);
}

TEST(SnapshotPool, AcquireIsRoundRobin)
{
  auto pool = makePool(3);
  PoolSlot* first = pool->tryAcquire();
  SnapshotPool::release(first);
  PoolSlot* second = pool->tryAcquire();
  // a freed slot is not immediately reused when others are free: the scan
  // continues from the last index, spreading wear and keeping recently
  // released slots readable a little longer for slow sinks.
  ASSERT_NE(second, first);
  SnapshotPool::release(second);
}

TEST(SnapshotPool, AcquireDoesNotAllocate)
{
  auto pool = makePool(8);
  AllocCounter::Scope scope;
  for(int i = 0; i < 1000; i++)
  {
    PoolSlot* s = pool->tryAcquire();
    ASSERT_NE(s, nullptr);
    s->snapshot.payload.resize(100);  // within capacity
    SnapshotPool::release(s);
  }
  ASSERT_EQ(scope.allocations(), 0u);
  ASSERT_EQ(scope.deallocations(), 0u);
}

TEST(SnapshotRef, IsMoveOnlyAndReleasesOnDestruction)
{
  static_assert(!std::is_copy_constructible_v<SnapshotRef>);
  static_assert(std::is_nothrow_move_constructible_v<SnapshotRef>);
  auto pool = makePool();
  PoolSlot* s = pool->tryAcquire();
  {
    SnapshotRef ref(pool, s);
    ASSERT_TRUE(ref);
    ASSERT_EQ(&*ref, &s->snapshot);
    ASSERT_EQ(s->refs.load(), 1u);
    SnapshotRef moved(std::move(ref));
    ASSERT_FALSE(ref);
    ASSERT_TRUE(moved);
    ASSERT_EQ(s->refs.load(), 1u);
  }
  ASSERT_EQ(s->refs.load(), 0u);
  ASSERT_EQ(pool->inUse(), 0u);
}

TEST(SnapshotRef, CloneAddsAReference)
{
  auto pool = makePool();
  PoolSlot* s = pool->tryAcquire();
  SnapshotRef a(pool, s);
  {
    SnapshotRef b = a.clone();
    ASSERT_EQ(s->refs.load(), 2u);
  }
  ASSERT_EQ(s->refs.load(), 1u);
  a.reset();
  ASSERT_EQ(s->refs.load(), 0u);
  ASSERT_FALSE(a);
}

TEST(SnapshotRef, KeepsPoolAliveAfterOwnerDropsIt)
{
  SnapshotRef survivor;
  {
    auto pool = makePool();
    PoolSlot* s = pool->tryAcquire();
    s->snapshot.payload.assign({ 1, 2, 3 });
    survivor = SnapshotRef(pool, s);
  }  // pool shared_ptr dropped here; the ref must keep it alive
  ASSERT_TRUE(survivor);
  ASSERT_EQ(survivor->payload.size(), 3u);
  survivor.reset();  // last reference: pool freed here (ASAN verifies)
}

// One producer acquiring slots and handing refs to N consumers that release
// them; run under TSAN. Also checks the "producer keeps its own hold until
// all consumers have a ref" protocol from spec §3 step 7.
TEST(SnapshotPool, ProducerAndConsumersUnderContention)
{
  constexpr int kConsumers = 3;
  constexpr int kRounds = 20000;
  auto pool = makePool(8);

  std::vector<std::vector<SnapshotRef>> mailboxes(kConsumers);
  std::vector<std::mutex> mailbox_mutex(kConsumers);
  std::atomic_bool done{ false };
  std::atomic<long> consumed{ 0 };

  std::vector<std::thread> consumers;
  for(int c = 0; c < kConsumers; c++)
  {
    consumers.emplace_back([&, c] {
      bool final_sweep = false;  // done is set after the last push: swap once more
      while(true)
      {
        std::vector<SnapshotRef> batch;
        {
          std::lock_guard lk(mailbox_mutex[c]);
          batch.swap(mailboxes[c]);
        }
        if(batch.empty())
        {
          if(final_sweep)
          {
            return;
          }
          if(done)
            final_sweep = true;
          else
            std::this_thread::yield();
          continue;
        }
        for(auto& ref : batch)
        {
          // read the payload the producer wrote (EXPECT: ASSERT cannot abort
          // a thread body, only the enclosing function)
          EXPECT_EQ(ref->payload.size(), 8u);
          consumed++;
        }
      }  // refs released when batch is destroyed
    });
  }

  long produced = 0;
  long dropped = 0;
  for(int i = 0; i < kRounds; i++)
  {
    PoolSlot* s = pool->tryAcquire();
    if(!s)
    {
      dropped++;
      std::this_thread::yield();
      continue;
    }
    s->snapshot.payload.resize(8);
    s->snapshot.payload[0] = uint8_t(i);
    // producer holds refs == 1 while handing out clones
    for(int c = 0; c < kConsumers; c++)
    {
      SnapshotPool::addRef(s);
      std::lock_guard lk(mailbox_mutex[c]);
      mailboxes[c].emplace_back(pool, s);
    }
    SnapshotPool::release(s);  // producer's own hold, released last
    produced++;
  }
  done = true;
  for(auto& t : consumers)
  {
    t.join();
  }
  ASSERT_EQ(consumed.load(), produced * kConsumers);
  ASSERT_EQ(pool->inUse(), 0u);
  ASSERT_EQ(pool->exhausted(), uint64_t(dropped));
}

TEST(SnapshotPool, ParentPinsSlotBetweenCompletedAndPendingFanout)
{
  auto pool = std::make_shared<SnapshotPool>(1, 8, 1);
  SnapshotRef parent(pool, pool->tryAcquire());
  auto first = parent.clone();
  first.reset();  // A fast consumer finishes before the next clone is made.
  EXPECT_EQ(pool->tryAcquire(), nullptr);
  auto second = parent.clone();
  parent.reset();
  EXPECT_EQ(pool->tryAcquire(), nullptr);
  second.reset();
  SnapshotRef reused(pool, pool->tryAcquire());
  EXPECT_TRUE(reused);
}
