#include "data_tamer/details/shared_state.hpp"
#include "alloc_counter.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

using namespace DataTamer;
using DataTamerTest::AllocCounter;

TEST(ChannelSharedState, SeriesStartEnabledAndMaskStartsDirty)
{
  ChannelSharedState state;
  ASSERT_TRUE(state.mask_dirty.load());
  state.addSeries();
  state.addSeries();
  ASSERT_EQ(state.seriesCount(), 2u);
  ASSERT_TRUE(state.isEnabled(0));
  ASSERT_TRUE(state.isEnabled(1));
}

TEST(ChannelSharedState, SetEnabledFlipsFlagsAndDirtiesMaskOnlyOnChange)
{
  ChannelSharedState state;
  for(int i = 0; i < 4; i++)
  {
    state.addSeries();
  }
  state.mask_dirty.store(false);

  state.setEnabled(1, false);
  state.setEnabled(2, false);
  ASSERT_TRUE(state.isEnabled(0));
  ASSERT_FALSE(state.isEnabled(1));
  ASSERT_FALSE(state.isEnabled(2));
  ASSERT_TRUE(state.isEnabled(3));
  ASSERT_TRUE(state.mask_dirty.exchange(false));

  // same value again: no change, mask stays clean
  state.setEnabled(1, false);
  ASSERT_FALSE(state.mask_dirty.load());

  state.setEnabled(2, true);
  ASSERT_TRUE(state.isEnabled(2));
  ASSERT_TRUE(state.mask_dirty.load());
}

TEST(ChannelSharedState, SetEnabledDoesNotAllocateOrLock)
{
  ChannelSharedState state;
  for(int i = 0; i < 8; i++)
  {
    state.addSeries();
  }
  AllocCounter::Scope scope;
  for(int i = 0; i < 1000; i++)
  {
    for(size_t index = 0; index < 8; index++)
    {
      state.setEnabled(index, (i & 1) != 0);
    }
  }
  ASSERT_EQ(scope.allocations(), 0u);
}

TEST(ChannelSharedState, WriteMutexIsUsableWithLockGuard)
{
  ChannelSharedState state;
  {
    std::lock_guard<WriteMutex> lk(state.write_mutex);
  }
  ASSERT_TRUE(state.write_mutex.try_lock());
  state.write_mutex.unlock();
}

// A toggling thread and a reading thread on the same flags; the reader must
// only ever observe true/false (never torn) and the run must be TSAN-clean.
TEST(ChannelSharedState, ConcurrentToggleAndReadIsRaceFree)
{
  ChannelSharedState state;
  for(int i = 0; i < 16; i++)
  {
    state.addSeries();
  }
  std::atomic_bool stop{ false };
  std::thread toggler([&] {
    bool v = false;
    while(!stop)
    {
      for(size_t index = 0; index < 16; index++)
      {
        state.setEnabled(index, v);
      }
      v = !v;
    }
  });
  for(int n = 0; n < 100000; n++)
  {
    if(state.mask_dirty.exchange(false, std::memory_order_seq_cst))
    {
      for(size_t i = 0; i < 16; i++)
      {
        const bool e = state.isEnabled(i);
        ASSERT_TRUE(e == true || e == false);
      }
    }
  }
  stop = true;
  toggler.join();
}

TEST(ChannelSharedState, EnableUpdatesNeverRestoreRegistration)
{
  ChannelSharedState state;
  state.addSeries();
  state.setUnregistered(0);
  EXPECT_FALSE(state.isRegistered(0));
  for(bool enabled : { false, true, false, true })
  {
    state.setEnabled(0, enabled);
    EXPECT_FALSE(state.isEnabled(0));
    EXPECT_FALSE(state.isRegistered(0));
  }
  EXPECT_EQ(state.setReregistered(0), 2u);
  EXPECT_TRUE(state.isRegistered(0));
  EXPECT_TRUE(state.isEnabled(0));
  EXPECT_TRUE(state.mask_dirty.exchange(false, std::memory_order_seq_cst));
}

// Exhausting a slot's generation counter is refused before it can wrap to the
// never-valid generation zero.
TEST(ChannelSharedState, GenerationExhaustionIsRefused)
{
  ChannelSharedState state;
  state.addSeries();
  while(state.canReregister(0))
  {
    state.setReregistered(0);
  }
  EXPECT_EQ(state.generation(0), ChannelSharedState::kMaxGeneration);
  EXPECT_THROW(state.setReregistered(0), std::length_error);
  EXPECT_EQ(state.generation(0), ChannelSharedState::kMaxGeneration);
  EXPECT_TRUE(state.isEnabled(0));
}

// Reads of existing flags and of the count race with appends without a lock.
TEST(ChannelSharedState, ReadsAreSafeWhileTheTableGrows)
{
  ChannelSharedState state;
  state.addSeries();
  std::atomic<bool> stop{ false };
  std::thread reader([&] {
    while(!stop)
    {
      const size_t count = state.seriesCount();
      for(size_t i = 0; i < count; ++i)
      {
        state.setEnabled(i, (i & 1) != 0);
      }
      (void)state.isEnabled(0);
    }
  });
  for(int i = 0; i < 5000; ++i)
  {
    state.addSeries();
  }
  stop = true;
  reader.join();
  EXPECT_EQ(state.seriesCount(), 5001u);
  EXPECT_TRUE(state.isRegistered(5000));
}

// The generation shares the atomic word with the flags: unregistering keeps
// it, re-registering bumps it, and enable toggles never disturb it.
TEST(ChannelSharedState, GenerationLivesInTheFlagsWord)
{
  ChannelSharedState state;
  EXPECT_EQ(state.addSeries(), 1u);
  EXPECT_EQ(state.generation(0), 1u);
  state.setEnabled(0, false);
  state.setEnabled(0, true);
  EXPECT_EQ(state.generation(0), 1u);
  state.setUnregistered(0);
  EXPECT_EQ(state.generation(0), 1u);
  EXPECT_EQ(state.setReregistered(0), 2u);
  EXPECT_EQ(state.generation(0), 2u);
  EXPECT_TRUE(state.isEnabled(0));
  EXPECT_EQ(state.setReregistered(0), 3u);
}
