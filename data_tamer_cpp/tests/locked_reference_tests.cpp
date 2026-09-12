#include "data_tamer/details/locked_reference.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <mutex>
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

TEST(LockedReference, MutexAliasIsTheWriteMutex)
{
  static_assert(std::is_same_v<Mutex, WriteMutex>);
  Mutex m;
  std::lock_guard<Mutex> lk(m);  // BasicLockable still satisfied
}

TEST(LockedReference, MutablePtrLocksExclusivelyForItsLifetime)
{
  WriteMutex m;
  int value = 1;
  {
    MutablePtr<int> p(&value, &m);
    ASSERT_TRUE(p);
    ASSERT_FALSE(canLockFromAnotherThread(m));  // held by p
    *p = 2;
  }
  ASSERT_TRUE(m.try_lock());
  m.unlock();
  ASSERT_EQ(value, 2);
}

TEST(LockedReference, ConstPtrAlsoLocksExclusively)
{
  WriteMutex m;
  const int value = 7;
  {
    ConstPtr<int> p(&value, &m);
    ASSERT_EQ(*p, 7);
    ASSERT_FALSE(canLockFromAnotherThread(m));
  }
  ASSERT_TRUE(m.try_lock());
  m.unlock();
}

TEST(LockedReference, AtomicProxyWritesBackOnDestruction)
{
  std::atomic<double> target{ 1.5 };
  {
    AtomicProxy<double> p(&target);
    ASSERT_TRUE(p);
    ASSERT_EQ(*p, 1.5);
    *p += 1.0;
    ASSERT_EQ(target.load(), 1.5);  // not yet visible
  }
  ASSERT_EQ(target.load(), 2.5);
}

TEST(LockedReference, AtomicConstProxyHoldsACopy)
{
  std::atomic<int> target{ 3 };
  AtomicConstProxy<int> p(&target);
  target.store(4);
  ASSERT_EQ(*p, 3);
  ASSERT_TRUE(p);
}

TEST(LockedReference, MovedLockingPtrsUnlockOnlyThroughTheirFinalOwner)
{
  WriteMutex m;
  int value = 1;
  {
    MutablePtr<int> a(&value, &m);
    MutablePtr<int> b(std::move(a));
    ASSERT_FALSE(a);
    ASSERT_TRUE(b);
    MutablePtr<int> c(nullptr, nullptr);
    c = std::move(b);
    ASSERT_FALSE(b);
    ASSERT_FALSE(canLockFromAnotherThread(m));  // still held, by c
  }
  ASSERT_TRUE(canLockFromAnotherThread(m));
  {
    ConstPtr<int> a(&value, &m);
    ConstPtr<int> b(std::move(a));
    ASSERT_FALSE(a);
    ASSERT_FALSE(canLockFromAnotherThread(m));
  }
  ASSERT_TRUE(canLockFromAnotherThread(m));
}

TEST(LockedReference, MovedAtomicProxyCommitsExactlyOnce)
{
  std::atomic<int> first{ 1 };
  std::atomic<int> second{ 10 };
  {
    AtomicProxy<int> a(&first);
    *a = 2;
    AtomicProxy<int> b(std::move(a));
    ASSERT_FALSE(a);
    ASSERT_EQ(first.load(), 1);  // nothing committed yet
    AtomicProxy<int> c(&second);
    *c = 20;
    c = std::move(b);  // commits 20 to second, takes over first
    ASSERT_FALSE(b);
    ASSERT_EQ(second.load(), 20);
    ASSERT_EQ(first.load(), 1);
  }
  ASSERT_EQ(first.load(), 2);
  ASSERT_EQ(second.load(), 20);
}

TEST(LockedReference, NullProxiesAreFalse)
{
  AtomicProxy<int> a(nullptr);
  AtomicConstProxy<int> b(nullptr);
  MutablePtr<int> c(nullptr, nullptr);
  ASSERT_FALSE(a);
  ASSERT_FALSE(b);
  ASSERT_FALSE(c);
}
