#include "data_tamer/details/locked_reference.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <thread>
#include <type_traits>

using namespace DataTamer;

namespace
{
bool canLockFromAnotherThread(WriteMutex& mutex)
{
  bool acquired = false;
  std::thread probe([&] {
    acquired = mutex.try_lock();
    if(acquired)
    {
      mutex.unlock();
    }
  });
  probe.join();
  return acquired;
}
}  // namespace

TEST(LockedReference, GuardsAreNotMovable)
{
  static_assert(!std::is_move_constructible_v<MutablePtr<int>>);
  static_assert(!std::is_move_constructible_v<ConstPtr<int>>);
  static_assert(!std::is_copy_constructible_v<MutablePtr<int>>);
}

TEST(LockedReference, MutablePtrLocksExclusivelyForItsLifetime)
{
  ChannelSharedState state;
  int value = 1;
  {
    MutablePtr<int> p(&value, state);
    ASSERT_FALSE(canLockFromAnotherThread(state.write_mutex));  // held by p
    *p = 2;
  }
  ASSERT_TRUE(canLockFromAnotherThread(state.write_mutex));
  ASSERT_EQ(value, 2);
}

TEST(LockedReference, ConstPtrAlsoLocksExclusively)
{
  ChannelSharedState state;
  const int value = 7;
  {
    ConstPtr<int> p(&value, state);
    ASSERT_EQ(*p, 7);
    ASSERT_FALSE(canLockFromAnotherThread(state.write_mutex));
  }
  ASSERT_TRUE(canLockFromAnotherThread(state.write_mutex));
}

// Guards may outlive the transaction they were created in (a helper returning
// one, a guard stored in a container): the node is unlinked and the mutex is
// held until the last guard on that state is gone.
TEST(LockedReference, OutOfOrderDestructionKeepsTheMutexUntilTheLastGuard)
{
  ChannelSharedState state;
  int value = 1;
  {
    auto outer = std::make_unique<ChannelSharedState::Transaction>(state);
    MutablePtr<int> guard(&value, state);                       // younger, non-owning
    outer.reset();                                              // older owner dies first
    ASSERT_FALSE(canLockFromAnotherThread(state.write_mutex));  // guard inherited it
    *guard = 2;
  }
  ASSERT_TRUE(canLockFromAnotherThread(state.write_mutex));
  ASSERT_EQ(value, 2);

  // Two states, guards destroyed in creation order (not LIFO).
  ChannelSharedState other;
  auto first = std::make_unique<ChannelSharedState::Transaction>(state);
  auto second = std::make_unique<ChannelSharedState::Transaction>(other);
  first.reset();
  ASSERT_TRUE(canLockFromAnotherThread(state.write_mutex));
  ASSERT_FALSE(canLockFromAnotherThread(other.write_mutex));
  {
    ChannelSharedState::Transaction nested(other);  // chain still intact
  }
  second.reset();
  ASSERT_TRUE(canLockFromAnotherThread(other.write_mutex));
}

// A guard taken inside a transaction on the same state joins it instead of
// deadlocking, and does not release the mutex when it goes away.
TEST(LockedReference, GuardsNestInsideAnOuterTransaction)
{
  ChannelSharedState state;
  int value = 1;
  {
    ChannelSharedState::Transaction outer(state);
    {
      MutablePtr<int> p(&value, state);
      *p = 2;
      {
        ConstPtr<int> c(&value, state);
        ASSERT_EQ(*c, 2);
      }
    }
    ASSERT_FALSE(canLockFromAnotherThread(state.write_mutex));  // still held by outer
  }
  ASSERT_TRUE(canLockFromAnotherThread(state.write_mutex));
}
