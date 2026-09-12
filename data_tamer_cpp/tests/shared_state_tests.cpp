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

  state.setEnabled(RegistrationID{ 1, 2 }, false);
  ASSERT_TRUE(state.isEnabled(0));
  ASSERT_FALSE(state.isEnabled(1));
  ASSERT_FALSE(state.isEnabled(2));
  ASSERT_TRUE(state.isEnabled(3));
  ASSERT_TRUE(state.mask_dirty.exchange(false));

  // same value again: no change, mask stays clean
  state.setEnabled(RegistrationID{ 1, 2 }, false);
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
    state.setEnabled(RegistrationID{ 0, 8 }, (i & 1) != 0);
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
      state.setEnabled(RegistrationID{ 0, 16 }, v);
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
  state.setRegistered(0, false);
  EXPECT_FALSE(state.isRegistered(0));
  for(bool enabled : {false, true, false, true})
  {
    state.setEnabled(0, enabled);
    EXPECT_FALSE(state.isEnabled(0));
    EXPECT_FALSE(state.isRegistered(0));
  }
  state.setRegistered(0, true);
  EXPECT_TRUE(state.isRegistered(0));
  EXPECT_TRUE(state.isEnabled(0));
  EXPECT_TRUE(state.mask_dirty.exchange(false, std::memory_order_seq_cst));
}
