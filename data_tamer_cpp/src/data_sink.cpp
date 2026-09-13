#include "data_tamer/data_sink.hpp"
#include "data_tamer/details/snapshot_pool.hpp"
#include "ConcurrentQueue/blockingconcurrentqueue.h"

#include <atomic>
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

constexpr uint64_t kClosed = uint64_t{ 1 } << 63;
}  // namespace

//---------------- SnapshotRef ----------------

SnapshotRef::SnapshotRef(std::shared_ptr<SnapshotPool> pool, PoolSlot* slot)
  : pool_(std::move(pool)), slot_(slot)
{}

SnapshotRef::SnapshotRef(SnapshotRef&& other) noexcept
  : pool_(std::move(other.pool_)), slot_(other.slot_)
{
  other.slot_ = nullptr;
}

SnapshotRef& SnapshotRef::operator=(SnapshotRef&& other) noexcept
{
  if(this != &other)
  {
    reset();
    pool_ = std::move(other.pool_);
    slot_ = other.slot_;
    other.slot_ = nullptr;
  }
  return *this;
}

SnapshotRef::~SnapshotRef()
{
  reset();
}

SnapshotRef SnapshotRef::clone() const
{
  if(slot_)
  {
    SnapshotPool::addRef(slot_);
  }
  return SnapshotRef(pool_, slot_);
}

void SnapshotRef::reset()
{
  if(slot_)
  {
    SnapshotPool::release(slot_);
    slot_ = nullptr;
  }
  pool_.reset();
}

const Snapshot& SnapshotRef::operator*() const
{
  return slot_->snapshot;
}

const Snapshot* SnapshotRef::operator->() const
{
  return &slot_->snapshot;
}

//---------------- SinkWorker ----------------

struct SinkWorker::Pimpl
{
  explicit Pimpl(size_t capacity) : queue(capacity) {}

  // Caller holds store_mutex.
  void deliver(DataSink& sink)
  {
    try
    {
      sink.onSnapshot(current_ref);
    }
    catch(const std::exception& e)
    {
      recordError(e.what());
    }
    catch(...)
    {
      recordError("unknown exception");
    }
    current_ref.reset();
  }

  void recordError(const char* what)
  {
    errors.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lock(error_mutex);
    last_error = what;
  }

  void startThread(DataSink& sink)
  {
    thread = std::jthread([this, &sink](std::stop_token stop) {
      while(!stop.stop_requested())
      {
        std::unique_lock handoff(handoff_mutex);
        std::unique_lock lock(store_mutex);
        handoff.unlock();
        if(stop.stop_requested())
          break;
        if(queue.wait_dequeue_timed(current_ref, std::chrono::milliseconds(50)))
        {
          // Drain everything already queued under the mutex we hold; one
          // lock round-trip per wake-up instead of one per snapshot.
          do
          {
            deliver(sink);
          } while(queue.try_dequeue(current_ref));
        }
      }
    });
  }

  void joinThread()
  {
    if(thread.joinable())
    {
      thread.request_stop();
      thread.join();
    }
  }

  moodycamel::BlockingConcurrentQueue<SnapshotRef, QueueTraits> queue;
  // A drainer claims handoff during the worker's wait/callback, preventing
  // the worker from immediately barging back into store_mutex.
  std::mutex handoff_mutex;
  std::mutex store_mutex;
  SnapshotRef current_ref;
  std::atomic<uint64_t> admission{ 0 };
  std::atomic<uint64_t> errors{ 0 };
  std::mutex error_mutex;
  std::string last_error;
  std::jthread thread;  // its stop token replaces a run flag
  Delivery delivery = Delivery::Threaded;
};

SinkWorker::SinkWorker(std::unique_ptr<DataSink> sink, size_t queue_capacity,
                       Delivery delivery)
  : _p(new Pimpl(queue_capacity)), _sink(std::move(sink))
{
  if(!_sink)
    throw std::invalid_argument("SinkWorker: null sink");
  _p->delivery = delivery;
  if(delivery == Delivery::Threaded)
  {
    _p->startThread(*_sink);
  }
}

SinkWorker::~SinkWorker()
{
  stop();
}

std::unique_ptr<moodycamel::ProducerToken> SinkWorker::makeProducerToken()
{
  auto token = std::make_unique<moodycamel::ProducerToken>(_p->queue);
  if(!token->valid())
    throw std::bad_alloc();
  return token;
}

bool SinkWorker::tryPush(moodycamel::ProducerToken& token, SnapshotRef&& snapshot)
{
  // Announce first, check second: a transient increment on a closed sink is
  // withdrawn at once, and stop() waits for it like any other.
  struct AdmissionGuard
  {
    std::atomic<uint64_t>& admission;
    ~AdmissionGuard()
    {
      admission.fetch_sub(1, std::memory_order_release);
      admission.notify_all();  // cheap when nobody waits; wakes stop()
    }
  } guard{ _p->admission };
  if(_p->admission.fetch_add(1, std::memory_order_acq_rel) & kClosed)
    return false;
  // Failed try_enqueue leaves snapshot intact; its owner releases it once.
  return _p->queue.try_enqueue(token, std::move(snapshot));
}

void SinkWorker::addSchema(const Schema& schema)
{
  std::lock_guard handoff(_p->handoff_mutex);
  std::lock_guard lock(_p->store_mutex);
  _sink->onSchema(schema);
}

void SinkWorker::stop()
{
  _p->admission.fetch_or(kClosed, std::memory_order_acq_rel);
  // Wait until every admitted enqueue has finished: block on the counter instead
  // of spinning; each release notifies.
  for(auto state = _p->admission.load(std::memory_order_acquire); (state & ~kClosed) != 0;
      state = _p->admission.load(std::memory_order_acquire))
  {
    _p->admission.wait(state, std::memory_order_acquire);
  }
  _p->joinThread();
  drain();
}

void SinkWorker::start()
{
  if(_p->delivery == Delivery::Threaded && !_p->thread.joinable())
  {
    _p->startThread(*_sink);
  }
  _p->admission.fetch_and(~kClosed, std::memory_order_release);
}

void SinkWorker::drain()
{
  std::lock_guard handoff(_p->handoff_mutex);
  std::lock_guard lock(_p->store_mutex);
  while(_p->queue.try_dequeue(_p->current_ref))
  {
    _p->deliver(*_sink);
  }
}

uint64_t SinkWorker::errors() const
{
  return _p->errors.load(std::memory_order_relaxed);
}

std::string SinkWorker::lastError() const
{
  std::lock_guard lock(_p->error_mutex);
  return _p->last_error;
}

}  // namespace DataTamer
