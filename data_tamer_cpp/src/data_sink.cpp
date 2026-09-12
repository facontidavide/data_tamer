#include "data_tamer/data_sink.hpp"
#include "data_tamer/details/snapshot_pool.hpp"
#include "ConcurrentQueue/blockingconcurrentqueue.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <exception>
#include <mutex>
#include <new>
#include <thread>

namespace DataTamer
{
namespace
{
struct QueueTraits : moodycamel::ConcurrentQueueDefaultTraits
{
  static void* malloc(size_t size) { return ::operator new(size, std::nothrow); }
  static void free(void* pointer) { ::operator delete(pointer); }
};

// Only the queue-dispatched callback on this thread may retain current_ref.
thread_local const DataSinkBase* callback_sink = nullptr;
constexpr uint64_t kClosed = uint64_t{ 1 } << 63;
}  // namespace

struct DataSinkBase::Pimpl
{
  explicit Pimpl(size_t capacity) : queue(capacity) {}

  void deliver(DataSinkBase* self)
  {
    const auto previous = callback_sink;
    callback_sink = self;
    try
    {
      self->storeSnapshot(*current_ref);
    }
    catch(...)
    {
      store_errors.fetch_add(1, std::memory_order_relaxed);
    }
    callback_sink = previous;
    current_ref.reset();
  }

  moodycamel::BlockingConcurrentQueue<SnapshotRef, QueueTraits> queue;
  // A drainer claims handoff during the worker's wait/callback, preventing
  // the worker from immediately barging back into store_mutex.
  std::mutex handoff_mutex;
  std::mutex store_mutex;
  SnapshotRef current_ref;
  std::atomic<uint64_t> admission{ 0 };
  std::atomic<uint64_t> store_errors{ 0 };
  std::atomic_bool run{ true };
  std::thread thread;
};

DataSinkBase::DataSinkBase(size_t queue_capacity) : _p(new Pimpl(queue_capacity))
{
  // _p and all its members must exist before the worker can observe them.
  _p->thread = std::thread([this] {
    while(_p->run.load())
    {
      std::unique_lock handoff(_p->handoff_mutex);
      std::unique_lock lock(_p->store_mutex);
      handoff.unlock();
      if(!_p->run.load())
        break;
      if(_p->queue.wait_dequeue_timed(_p->current_ref, std::chrono::milliseconds(50)))
      {
        // Drain everything already queued under the mutex we hold; one
        // lock round-trip per wake-up instead of one per snapshot.
        do
        {
          _p->deliver(this);
        } while(_p->queue.try_dequeue(_p->current_ref));
      }
    }
  });
}

DataSinkBase::~DataSinkBase()
{
  if(_p->thread.joinable())
  {
    // Construction unwinding must preserve the original exception.
    if(std::uncaught_exceptions() == 0)
    {
      std::fputs("DataSinkBase: derived destructor must call stopThread()\n", stderr);
      assert(false && "derived destructor must call stopThread()");
    }
    stopThread();
  }
}

std::unique_ptr<moodycamel::ProducerToken> DataSinkBase::makeProducerToken()
{
  auto token = std::make_unique<moodycamel::ProducerToken>(_p->queue);
  if(!token->valid())
    throw std::bad_alloc();
  return token;
}

bool DataSinkBase::tryPush(moodycamel::ProducerToken& token, SnapshotRef&& snapshot)
{
  // Announce first, check second: a transient increment on a closed sink is
  // withdrawn at once, and stopAcceptingSnapshots() waits for it like any other.
  struct AdmissionGuard
  {
    std::atomic<uint64_t>& admission;
    ~AdmissionGuard() { admission.fetch_sub(1, std::memory_order_release); }
  } guard{ _p->admission };
  if(_p->admission.fetch_add(1, std::memory_order_acq_rel) & kClosed)
    return false;
  // Failed try_enqueue leaves snapshot intact; its owner releases it once.
  return _p->queue.try_enqueue(token, std::move(snapshot));
}

void DataSinkBase::stopAcceptingSnapshots()
{
  _p->admission.fetch_or(kClosed, std::memory_order_acq_rel);
  while((_p->admission.load(std::memory_order_acquire) & ~kClosed) != 0)
  {
    std::this_thread::yield();
  }
}

void DataSinkBase::startAcceptingSnapshots()
{
  _p->admission.fetch_and(~kClosed, std::memory_order_release);
}

void DataSinkBase::processQueuedSnapshots()
{
  std::lock_guard handoff(_p->handoff_mutex);
  std::lock_guard lock(_p->store_mutex);
  while(_p->queue.try_dequeue(_p->current_ref))
  {
    _p->deliver(this);
  }
}

void DataSinkBase::stopThread()
{
  _p->run.store(false);
  if(_p->thread.joinable())
    _p->thread.join();
}

uint64_t DataSinkBase::storeErrors() const
{
  return _p->store_errors.load(std::memory_order_relaxed);
}

SnapshotRef DataSinkBase::retainSnapshot() const
{
  assert(callback_sink == this && "retainSnapshot is only valid inside a queued "
                                  "callback");
  if(callback_sink != this)
    return {};
  return _p->current_ref.clone();
}

}  // namespace DataTamer
