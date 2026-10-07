#include "data_tamer/data_sink.hpp"
#include "data_tamer/details/snapshot_pool.hpp"
#include "data_tamer/details/spin_pause.hpp"

#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <mutex>
#include <semaphore>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace DataTamer
{
namespace
{
constexpr uint64_t kClosed = uint64_t{ 1 } << 63;
// Rounds over the attached queues in one delivery pass, before the worker looks
// again for attachments that came or went.
constexpr size_t kRoundsPerPass = 64;

// How long an idle worker keeps polling for a push before it marks itself
// asleep. While it polls, post() on the snapshot thread is an increment and a
// load; once it sleeps, the next post() releases the semaphore, a futex wake on
// the snapshot thread. 20 us covers the snapshots one control tick takes of
// several channels (each a few microseconds of serialization apart), so such a
// burst costs at most one wake. The price is up to 20 us of one core per idle
// transition of the worker: 2 % of a core for a worker woken once per 1 kHz
// tick.
constexpr std::chrono::microseconds kSpinBeforeSleep{ 20 };

// Wakes the worker thread without a lock. post() costs one atomic increment and
// one load; only the post() that finds the worker asleep releases the semaphore
// (one futex wake). The worker sleeps on the semaphore's own counter, not with
// std::atomic::wait: with libstdc++ 13 on Linux (_GLIBCXX_HAVE_PLATFORM_WAIT)
// that is a bare futex wait, outside the address-hashed waiter table that
// std::atomic::wait shares between addresses, so the channel epoch's
// notify_all() on the snapshot path never finds the sleeping worker in its
// bucket and stays a load, and release() takes no lock. Other standard
// libraries, or targets without a platform wait (where libstdc++'s release()
// takes the waiter pool mutex), are correct but were not analysed for the
// real-time path.
class WorkerWake
{
public:
  void post()
  {
    // seq_cst on both sides: either sleep() sees this increment, or this load
    // sees `sleeping` set and the release below wakes it.
    sequence_.fetch_add(1, std::memory_order_seq_cst);
    if(sleeping_.load(std::memory_order_seq_cst) &&
       sleeping_.exchange(false, std::memory_order_seq_cst))
    {
      semaphore_.release();
    }
  }

  [[nodiscard]] uint32_t observe() const
  {
    return sequence_.load(std::memory_order_seq_cst);
  }

  // Worker only: polls for kSpinBeforeSleep. True if post() was called after
  // observe() returned `observed`.
  [[nodiscard]] bool spin(uint32_t observed) const
  {
    const auto deadline = std::chrono::steady_clock::now() + kSpinBeforeSleep;
    for(uint32_t polls = 1;; ++polls)
    {
      if(observe() != observed)
      {
        return true;
      }
      details::spinPause();
      // Reading the clock (vDSO, no syscall) every 64 polls keeps it cheap.
      if(polls % 64 == 0 && std::chrono::steady_clock::now() >= deadline)
      {
        return false;
      }
    }
  }

  // Blocks unless post() was called after observe() returned `observed`.
  void sleep(uint32_t observed)
  {
    sleeping_.store(true, std::memory_order_seq_cst);
    if(sequence_.load(std::memory_order_seq_cst) != observed &&
       sleeping_.exchange(false, std::memory_order_seq_cst))
    {
      return;  // withdrawn before any post() saw it
    }
    // Either nothing was posted since `observed`, or a post() took `sleeping`
    // and releases (or released) the semaphore: one release per sleep.
    semaphore_.acquire();
  }

private:
  std::atomic<uint32_t> sequence_{ 0 };
  std::atomic<bool> sleeping_{ false };
  std::binary_semaphore semaphore_{ 0 };
};
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

/// Single-producer single-consumer ring of one channel on one worker. The
/// producer is the channel's snapshot thread (tryPush), the consumer whoever
/// holds store_mutex (the worker thread or drain()).
struct SinkWorker::Attachment
{
  // One entry more than the pool: an empty entry tells a full ring from an
  // empty one, without a division on the push path.
  explicit Attachment(size_t capacity)
    : size(capacity + 1), entries(std::make_unique<SnapshotRef[]>(capacity + 1))
  {}

  [[nodiscard]] size_t advance(size_t index) const
  {
    return index + 1 == size ? 0 : index + 1;
  }

  // Producer only. Never full in practice: every entry holds a distinct slot of
  // a pool of `size - 1` slots, and the snapshot being pushed holds one more.
  bool push(SnapshotRef&& snapshot)
  {
    const size_t at = tail.load(std::memory_order_relaxed);
    const size_t after = advance(at);
    if(after == cached_head)
    {
      cached_head = head.load(std::memory_order_acquire);
      if(after == cached_head)
      {
        return false;
      }
    }
    entries[at] = std::move(snapshot);  // the entry is empty: nothing is freed
    tail.store(after, std::memory_order_release);
    return true;
  }

  // Consumer only.
  bool pop(SnapshotRef& out)
  {
    const size_t at = head.load(std::memory_order_relaxed);
    if(at == cached_tail)
    {
      cached_tail = tail.load(std::memory_order_acquire);
      if(at == cached_tail)
      {
        return false;
      }
    }
    out = std::move(entries[at]);
    head.store(advance(at), std::memory_order_release);
    return true;
  }

  [[nodiscard]] bool empty() const
  {
    return head.load(std::memory_order_acquire) == tail.load(std::memory_order_acquire);
  }

  // Read-mostly line: written once, or once by detach().
  const size_t size;
  std::unique_ptr<SnapshotRef[]> entries;
  // Set by detach(), after the channel's last push.
  std::atomic<bool> detached{ false };
  // Consumer line: what the deliverer writes on every pop.
  alignas(64) std::atomic<size_t> head{ 0 };
  size_t cached_tail = 0;
  // Producer line, alone (the alignment pads the struct to whole lines).
  alignas(64) std::atomic<size_t> tail{ 0 };
  size_t cached_head = 0;
};

struct SinkWorker::Pimpl
{
  explicit Pimpl(std::unique_ptr<DataSink> owned) : sink(std::move(owned)) {}

  // handoff_mutex, then store_mutex: how every caller other than the worker
  // thread enters the store. Holding handoff while it waits for store_mutex
  // keeps the worker, which releases handoff once it holds store_mutex, from
  // barging back in after its current pass.
  struct StoreLock
  {
    explicit StoreLock(Pimpl& p) : handoff(p.handoff_mutex), store(p.store_mutex) {}
    std::lock_guard<std::mutex> handoff;
    std::lock_guard<std::mutex> store;
  };

  // What one delivery pass reports to its caller.
  struct Pass
  {
    // The last round over the queues found them all empty, and no queue was
    // attached during the pass.
    bool complete = false;
    // The wake sequence read before that last round: a push the round missed
    // posted after it.
    uint32_t observed = 0;
  };

  void recordError(const char* what)
  {
    errors.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lock(error_mutex);
    last_error = what;
  }

  // Runs a sink callback; a throw is counted (errors(), lastError()) instead of
  // propagating.
  template <typename Callback>
  void guarded(Callback&& callback)
  {
    try
    {
      std::forward<Callback>(callback)();
    }
    catch(const std::exception& e)
    {
      recordError(e.what());
    }
    catch(...)
    {
      recordError("unknown exception");
    }
  }

  // Caller holds store_mutex.
  void deliver()
  {
    guarded([this] { sink->onSnapshot(current_ref); });
    current_ref.reset();
  }

  // The sink's onStop(), once per stop, serialized with the other callbacks.
  void finishSink()
  {
    StoreLock lock(*this);
    if(!sink_running)
    {
      return;  // stopped already, and not started since
    }
    sink_running = false;
    guarded([this] { sink->onStop(); });
  }

  // One delivery pass; caller holds store_mutex. Allocates only when more
  // queues are attached than in any earlier pass (pass_list grows).
  Pass deliverPass()
  {
    {
      std::lock_guard lock(attachments_mutex);
      for(const auto& attachment : attachments)
      {
        pass_list.push_back(attachment.get());
      }
    }
    // Detached queues first, oldest first, each to the end (nothing is pushed
    // into them any more): their channel may already publish into a newer
    // queue on this worker, and per-channel order puts those snapshots after
    // these. removeDataSink() detaches under the channel's control mutex
    // before a later addDataSink() attaches, and attach() takes
    // attachments_mutex, so a newer queue in pass_list implies that its
    // predecessors read as detached here. A queue detached later in this pass
    // has no successor in pass_list, so serving it in the round robin below
    // keeps the order too.
    for(Attachment* attachment : pass_list)
    {
      if(attachment->detached.load(std::memory_order_acquire))
      {
        while(attachment->pop(current_ref))
        {
          deliver();
        }
      }
    }
    // Round robin, one entry per queue and round, until a round finds every
    // queue empty. Each round reads the wake sequence first. A push stores its
    // tail (release) before its post() increments the sequence (seq_cst), so
    // a round whose read saw that increment also sees the entry: a push that
    // the last, empty round missed posted after `observed`, and spin() or
    // sleep() return at once instead of a second pass checking for it.
    Pass pass;
    bool empty_round = false;
    for(size_t round = 0; round < kRoundsPerPass && !empty_round; ++round)
    {
      pass.observed = wake.observe();
      empty_round = true;
      for(Attachment* attachment : pass_list)
      {
        if(attachment->pop(current_ref))
        {
          deliver();
          empty_round = false;
        }
      }
    }
    std::lock_guard lock(attachments_mutex);
    // Only attach() adds and only a pass removes: a longer list means a queue
    // attached after pass_list was taken, which the empty round did not look
    // at although its pushes may have posted before `observed`.
    pass.complete = empty_round && attachments.size() == pass_list.size();
    pass_list.clear();
    // detach() comes after the channel's last push, so detached and empty
    // means drained: free those queues.
    std::erase_if(attachments, [](const std::unique_ptr<Attachment>& attachment) {
      return attachment->detached.load(std::memory_order_acquire) && attachment->empty();
    });
    return pass;
  }

  void startThread()
  {
    thread = std::jthread([this](std::stop_token stop) {
      while(!stop.stop_requested())
      {
        Pass pass;
        {
          std::unique_lock handoff(handoff_mutex);
          std::unique_lock lock(store_mutex);
          handoff.unlock();
          pass = deliverPass();
        }
        // An incomplete pass runs again at once. joinThread() requests the
        // stop before its post(): a post() before `observed` makes the stop
        // visible here, a later one ends spin() or sleep().
        if(pass.complete && !stop.stop_requested() && !wake.spin(pass.observed))
        {
          wake.sleep(pass.observed);
        }
      }
    });
  }

  void joinThread()
  {
    if(thread.joinable())
    {
      thread.request_stop();
      wake.post();
      thread.join();
    }
  }

  // First member, destroyed last: after the thread and every queued snapshot.
  std::unique_ptr<DataSink> sink;
  // Worker side: written by whoever delivers.
  std::mutex handoff_mutex;
  std::mutex store_mutex;
  SnapshotRef current_ref;  // store_mutex
  // store_mutex: false from onStop() until start().
  bool sink_running = true;
  // The owned queues, attached and detached-but-not-yet-drained, oldest first.
  // Leaf lock: taken under store_mutex and under LogChannel's control mutex,
  // never the reverse.
  std::mutex attachments_mutex;
  std::vector<std::unique_ptr<Attachment>> attachments;
  // store_mutex: the queues of the current pass, walked without
  // attachments_mutex. Cleared after each pass, its capacity kept.
  std::vector<Attachment*> pass_list;
  std::atomic<uint64_t> errors{ 0 };
  std::mutex error_mutex;
  std::string last_error;
  std::jthread thread;  // its stop token replaces a run flag
  Delivery delivery = Delivery::Threaded;
  // Written by every push, each on a line of its own (the alignment pads the
  // struct to whole lines). Posted after every push, and by detach() and stop().
  alignas(64) WorkerWake wake;
  alignas(64) std::atomic<uint64_t> admission{ 0 };
};

SinkWorker::SinkWorker(std::unique_ptr<DataSink> sink, Delivery delivery)
{
  if(!sink)
  {
    throw std::invalid_argument("SinkWorker: null sink");
  }
  _p = std::make_unique<Pimpl>(std::move(sink));
  _p->delivery = delivery;
  if(delivery == Delivery::Threaded)
  {
    _p->startThread();
  }
}

SinkWorker::~SinkWorker()
{
  stop();
}

DataSink& SinkWorker::sink()
{
  return *_p->sink;
}

const DataSink& SinkWorker::sink() const
{
  return *_p->sink;
}

SinkWorker::Attachment* SinkWorker::attach(size_t capacity)
{
  auto attachment = std::make_unique<Attachment>(capacity);
  Attachment* attached = attachment.get();
  std::lock_guard lock(_p->attachments_mutex);
  _p->attachments.push_back(std::move(attachment));  // appended: oldest first
  return attached;
}

void SinkWorker::detach(Attachment* attachment) noexcept
{
  attachment->detached.store(true, std::memory_order_release);
  _p->wake.post();  // the worker delivers what is left and frees the queue
}

bool SinkWorker::tryPush(Attachment& attachment, SnapshotRef&& snapshot)
{
  // Announce first, check second: a transient increment on a closed sink is
  // withdrawn at once, and stop() waits for it like any other.
  struct AdmissionGuard
  {
    std::atomic<uint64_t>& admission;
    ~AdmissionGuard()
    {
      // stop() waits only after its fetch_or(kClosed). A decrement that reads
      // kClosed clear precedes that fetch_or in the counter's modification
      // order, so the loads stop() makes afterwards see it already: only a
      // decrement that reads kClosed set can have a waiter to wake.
      if(admission.fetch_sub(1, std::memory_order_release) & kClosed)
      {
        admission.notify_all();
      }
    }
  } guard{ _p->admission };
  if(_p->admission.fetch_add(1, std::memory_order_acq_rel) & kClosed)
  {
    return false;
  }
  // A failed push leaves snapshot intact; its owner releases it once.
  if(!attachment.push(std::move(snapshot)))
  {
    return false;
  }
  _p->wake.post();
  return true;
}

void SinkWorker::addSchema(const Schema& schema)
{
  Pimpl::StoreLock lock(*_p);
  _p->sink->onSchema(schema);
}

void SinkWorker::stop()
{
  _p->admission.fetch_or(kClosed, std::memory_order_acq_rel);
  // Wait until every admitted push has finished: block on the counter instead
  // of spinning; a release made after the close notifies.
  for(auto state = _p->admission.load(std::memory_order_acquire); (state & ~kClosed) != 0;
      state = _p->admission.load(std::memory_order_acquire))
  {
    _p->admission.wait(state, std::memory_order_acquire);
  }
  _p->joinThread();
  drain();
  _p->finishSink();
}

void SinkWorker::start()
{
  {
    Pimpl::StoreLock lock(*_p);
    if(!_p->sink_running)
    {
      _p->sink_running = true;  // the next stop() calls onStop() again
      _p->guarded([this] { _p->sink->onStart(); });
    }
  }
  if(_p->delivery == Delivery::Threaded && !_p->thread.joinable())
  {
    _p->startThread();
  }
  _p->admission.fetch_and(~kClosed, std::memory_order_release);
}

void SinkWorker::drain()
{
  Pimpl::StoreLock lock(*_p);
  // A complete pass ends with a round that found every queue empty, so what
  // was pushed before the call has been delivered.
  while(!_p->deliverPass().complete)
  {
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
