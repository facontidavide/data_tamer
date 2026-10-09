#include "data_tamer/channel.hpp"
#include "data_tamer/data_sink.hpp"
#include "data_tamer/contrib/SerializeMe.hpp"
#include "data_tamer/data_tamer.hpp"
#include "data_tamer/details/shared_state.hpp"
#include "data_tamer/details/snapshot_pool.hpp"
#include "waiter_count.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace DataTamer
{
namespace
{
// Not constexpr: std::vector::max_size() is constexpr only from libstdc++ 12.
const size_t kMaxPayloadBytes = PayloadVector().max_size();
// Largest pool LogChannel::setPoolCapacity() accepts: its slot array must be
// addressable.
constexpr size_t kMaxPoolSlots =
    std::numeric_limits<std::ptrdiff_t>::max() / sizeof(PoolSlot);

size_t checkedDouble(size_t size)
{
  if(size > kMaxPayloadBytes / 2)
  {
    throw std::length_error("Snapshot payload is too large to reserve");
  }
  return 2 * size;
}

std::string ChannelPrefix(const std::string& channel_name)
{
  return "channel '" + channel_name + "': ";
}

// The name with the bytes it cannot hold shown as \xNN, so an error stays on one line.
std::string Printable(std::string_view name)
{
  static constexpr char kHex[] = "0123456789abcdef";
  std::string text;
  for(const char c : name)
  {
    const auto byte = static_cast<unsigned char>(c);
    if(byte != ' ' && details::IsForbiddenNameByte(byte))
    {
      text += "\\x";
      text += kHex[byte >> 4];
      text += kHex[byte & 0xF];
    }
    else
    {
      text += c;
    }
  }
  return text;
}

// Why `name` cannot be written into the schema text, or nullptr if it can. The text has
// one item per line and a field line splits at its first space, so a name is not empty
// and holds no whitespace or control character. A channel name (`allow_space`) fills
// its header line and can hold spaces.
const char* NameProblem(std::string_view name, bool allow_space = false)
{
  if(name.empty())
  {
    return "it is empty";
  }
  const size_t bad = details::FindForbiddenNameByte(name, allow_space);
  if(bad == std::string_view::npos)
  {
    return nullptr;
  }
  if(name[bad] == ' ')
  {
    return "it contains a space";
  }
  return allow_space ? "it contains a control character" :
                       "it contains a whitespace or control character";
}

// Throws std::runtime_error if `name`, a `what` ("value name", ...) of the channel's
// schema, cannot be written into the schema text. A field name also names its type.
void CheckSchemaName(const std::string& channel_name, const char* what,
                     std::string_view name, std::string_view in_type = {})
{
  const char* problem = NameProblem(name);
  if(problem == nullptr)
  {
    return;
  }
  std::string message =
      ChannelPrefix(channel_name) + "invalid " + what + " '" + Printable(name) + "'";
  if(!in_type.empty())
  {
    message += " in custom type '" + Printable(in_type) + "'";
  }
  throw std::runtime_error(message + ": " + problem);
}
}  // namespace

struct LogChannel::Pimpl
{
  struct ValueHolder
  {
    std::string name;
    ValuePtr holder;
  };
  std::string channel_name;
  TypesRegistry type_registry;  // control_mutex
  mutable std::mutex control_mutex;
  std::vector<ValueHolder> series;
  std::unordered_map<std::string, size_t> registered_values;
  std::shared_ptr<ChannelSharedState> shared = std::make_shared<ChannelSharedState>();
  std::atomic<uint64_t> write_lock_contended{ 0 };
  std::atomic<uint64_t> write_lock_wait_max_ns{ 0 };
  ActiveMask active_mask;  // write mutex held; independent of retained slots
  size_t payload_capacity = 0;
  size_t pool_capacity = SnapshotPool::kDefaultCapacity;
  std::atomic<uint64_t> payload_reallocations{ 0 };
  std::atomic<uint64_t> dropped_oversize{ 0 };
  std::atomic<uint64_t> attempts{ 0 };  // takeSnapshot()/tryTakeSnapshot() calls
  std::atomic<uint64_t> accepted{ 0 };  // ... that at least one sink took
  std::shared_ptr<SnapshotPool> pool;
  Schema schema;
  bool schema_frozen = false;  // control_mutex held
  // Set once by startLogging() after the pool and every sink announcement succeeded;
  // read without locks by the snapshot thread.
  std::atomic<bool> logging_started{ false };
  std::mutex start_mutex;  // serializes startLogging(), which releases control_mutex

  /// Schema changed while open: every sink must hear it again.
  void invalidateAnnouncements()
  {
    forEachLink([](SinkLink& link) { link.schema_registered = false; });
  }

  void rebuildMask()
  {
    std::fill(active_mask.begin(), active_mask.end(), 0xFF);
    for(size_t i = 0; i < series.size(); ++i)
    {
      if(!shared->isEnabled(i))
      {
        SetBit(active_mask, i, false);
      }
    }
  }

  // Reader side of the mask handshake. Both operations are SC, so a reader that
  // saw the flag clear is ordered before any later dirty store and is covered by
  // waitQuiescent(); the plain load keeps the common path free of a locked RMW.
  void refreshMaskIfDirty()
  {
    if(shared->mask_dirty.load(std::memory_order_seq_cst) &&
       shared->mask_dirty.exchange(false, std::memory_order_seq_cst))
    {
      rebuildMask();
    }
  }

  // Caller holds write_mutex; size and serialization use this same cached mask.
  // Sums the enabled values, or every value with kAllValues. Returns SIZE_MAX
  // as soon as the sum exceeds `limit` (the RT path passes the slot capacity
  // and never throws); with the default limit it throws instead.
  template <bool kAllValues = false>
  size_t payloadSize(size_t limit = kMaxPayloadBytes) const
  {
    size_t size = 0;
    for(size_t i = 0; i < series.size(); ++i)
    {
      if(kAllValues || GetBit(active_mask, i))
      {
        const auto field_size = series[i].holder.getSerializedSize();
        if(field_size > limit - size)
        {
          if(limit == kMaxPayloadBytes)
          {
            throw std::length_error("Snapshot payload is too large");
          }
          return std::numeric_limits<size_t>::max();
        }
        size += field_size;
      }
    }
    return size;
  }

  // Per-slot payload reservation made by startLogging(); caller holds write_mutex
  // with the mask rebuilt. When every value is fixed-size, exactly the payload
  // with all of them enabled (one disabled now may be enabled later). Otherwise
  // twice the current payload and at least 256 bytes, so that containers can
  // grow before tryTakeSnapshot() reports `oversize`. payload_capacity is a floor.
  size_t initialPayloadCapacity() const
  {
    const bool fixed = std::all_of(series.begin(), series.end(), [](const auto& value) {
      return value.holder.isFixedSize();
    });
    if(!fixed)
    {
      return std::max({ payload_capacity, checkedDouble(payloadSize()), size_t(256) });
    }
    return std::max(payload_capacity, payloadSize<true>());
  }

  struct SinkLink
  {
    explicit SinkLink(std::shared_ptr<SinkWorker> worker) : sink(std::move(worker)) {}
    ~SinkLink() { dropQueue(); }
    SinkLink(const SinkLink&) = delete;
    SinkLink& operator=(const SinkLink&) = delete;

    /// Give the link a queue on its worker, unless it has one. Control path:
    /// allocates, may throw (link unchanged).
    void ensureQueue(size_t pool_capacity)
    {
      if(!queue)
      {
        queue = sink->attach(pool_capacity);
      }
    }

    /// End the attachment: the worker still delivers what it holds, then frees it.
    void dropQueue() noexcept
    {
      if(queue)
      {
        sink->detach(queue);
        queue = nullptr;
      }
    }

    std::shared_ptr<SinkWorker> sink;
    // Allocated right before the link is published; every published link has one.
    SinkWorker::Attachment* queue = nullptr;
    std::atomic<uint64_t> dropped{ 0 };
    bool schema_registered = false;
  };
  static constexpr size_t kMaxSinks = LogChannel::kMaxSinks;
  std::array<std::unique_ptr<SinkLink>, kMaxSinks> sinks;
  std::array<std::atomic<SinkLink*>, kMaxSinks> published_sinks{};
  // Workers addDataSink() is announcing to with control_mutex released: another
  // addDataSink() of one of them waits on `announced` (with control_mutex).
  std::vector<const SinkWorker*> announcing;
  std::condition_variable announced;
  std::atomic<uint64_t> epoch{ 0 };
  details::WaiterCount quiescence_waiters;  // controllers in waitQuiescent()

  // Caller holds control_mutex (or owns the channel exclusively).
  template <typename Function>
  void forEachLink(Function&& function)
  {
    for(auto& link : sinks)
    {
      if(link)
      {
        function(*link);
      }
    }
  }

  // Slot of the link to `worker`, kMaxSinks if it is not attached. Caller
  // holds control_mutex.
  [[nodiscard]] size_t findLink(const std::shared_ptr<SinkWorker>& worker) const
  {
    for(size_t i = 0; i < sinks.size(); ++i)
    {
      if(sinks[i] && sinks[i]->sink == worker)
      {
        return i;
      }
    }
    return kMaxSinks;
  }

  // Caller holds control_mutex.
  [[nodiscard]] bool isAnnouncing(const SinkWorker* worker) const
  {
    return std::find(announcing.begin(), announcing.end(), worker) != announcing.end();
  }

  // Gives sinks[i] its queue on the worker, as many entries as the pool has
  // slots, then publishes the link to the snapshot thread. Caller holds
  // control_mutex, and startLogging() created the pool. Throws (std::bad_alloc)
  // before anything is published.
  void publish(size_t i)
  {
    sinks[i]->ensureQueue(pool->capacity());
    published_sinks[i].store(sinks[i].get(), std::memory_order_seq_cst);
  }

  // Controller holds control_mutex. SC order prevents a reader from both
  // seeing an old link/cached mask and being missed by this epoch observation:
  // its entry precedes its old-pointer load/dirty exchange, which precedes
  // removal's SC publication and this load. Wait only for that reader; later
  // readers see the new publication. Exit is release, observation is acquire.
  void waitQuiescent()
  {
    details::WaiterCount::Scope waiting(quiescence_waiters);  // before the epoch read
    const auto observed = epoch.load(std::memory_order_seq_cst);
    if(observed & 1)
    {
      epoch.wait(observed, std::memory_order_seq_cst);  // returns once the value changed
    }
  }

  // The workers whose sleep a snapshot's pushes took (SinkWorker::Push::wake_owed).
  // Declared before the snapshot's write lock and epoch guard, so destroyed after
  // them: the futex calls are made with the write mutex released, and writers do not
  // wait for them. Without the epoch a worker can be removed meanwhile, and a callback
  // that drops its last reference frees the SinkWorker at once (~SinkWorker() leaves
  // the stop to another thread). So each entry is the worker's Pimpl, taken inside the
  // epoch: both destructor paths free it only after joining the worker thread, which
  // sleeps until its wake (see WorkerWake::release()).
  struct OwedWakes
  {
    OwedWakes() = default;
    OwedWakes(const OwedWakes&) = delete;
    OwedWakes& operator=(const OwedWakes&) = delete;
    ~OwedWakes()
    {
      for(size_t i = 0; i < count; ++i)
      {
        SinkWorker::wake(*workers[i]);
      }
    }
    std::array<SinkWorker::Pimpl*, kMaxSinks> workers;
    size_t count = 0;
  };

  // Reader side of waitQuiescent(): odd epoch while a snapshot is in progress.
  struct EpochGuard
  {
    explicit EpochGuard(Pimpl& p) : epoch(p.epoch), waiters(p.quiescence_waiters)
    {
      epoch.fetch_add(1, std::memory_order_seq_cst);
    }
    ~EpochGuard()
    {
      epoch.fetch_add(1, std::memory_order_seq_cst);
      waiters.notifyIfWaiting(epoch);  // no futex call unless a controller waits
    }
    std::atomic<uint64_t>& epoch;
    details::WaiterCount& waiters;
  };
};

RegistrationID LogChannel::registerValueImpl(const std::string& name,
                                             ValuePtr&& value_ptr,
                                             CustomSerializer::Ptr type_info)
{
  return registerValueWithTypes(name, std::move(value_ptr), std::move(type_info), {});
}

RegistrationID LogChannel::registerValueWithTypes(const std::string& name,
                                                  ValuePtr&& value_ptr,
                                                  CustomSerializer::Ptr type_info,
                                                  PendingTypes&& types)
{
  // The public registration template holds control_mutex, including type
  // discovery, and has already validated the name with checkValueName().
  auto it = _p->registered_values.find(name);
  if(it == _p->registered_values.end())
  {
    if(_p->schema_frozen)
    {
      throw std::runtime_error(ChannelPrefix(_p->channel_name) +
                               "can't register new value '" + name +
                               "' once recording started");
    }
    // Whatever can throw and needs no change to the channel comes first (user code
    // such as typeSchema() included), then spare capacity for what is appended.
    const auto type = value_ptr.type();
    const std::string type_name = type_info ? type_info->typeName() : ToStr(type);
    // The type and field names this registration writes into the schema text.
    if(type_info)
    {
      CheckSchemaName(_p->channel_name, "custom type name", type_name);
    }
    for(const auto& [pending_name, pending_fields] : types)
    {
      CheckSchemaName(_p->channel_name, "custom type name", pending_name);
      for(const auto& pending_field : pending_fields)
      {
        CheckSchemaName(_p->channel_name, "field name", pending_field.field_name,
                        pending_name);
      }
    }
    TypeField field{ name, type, type_name, value_ptr.isVector(),
                     value_ptr.vectorSize() };
    std::optional<CustomSchema> custom_schema;
    if(type_info && !_p->schema.custom_types.contains(type_info->typeName()) &&
       !types.contains(type_info->typeName()))
    {
      custom_schema = type_info->typeSchema();
    }
    Pimpl::ValueHolder instance;
    instance.name = name;
    instance.holder = std::move(value_ptr);
    _p->series.reserve(_p->series.size() + 1);
    _p->schema.fields.reserve(_p->schema.fields.size() + 1);
    std::vector<decltype(_p->schema.custom_types)::iterator> added_types;
    added_types.reserve(types.size());

    // Then the changes, each of which either completes or throws without effect, in
    // an order that ends with the only one that cannot be undone, addSeries(). A throw
    // undoes the earlier ones, which cannot throw.
    const size_t index = _p->series.size();
    std::optional<decltype(_p->registered_values)::iterator> indexed;
    std::optional<decltype(_p->schema.custom_schemas)::iterator> added_schema;
    bool appended = false;
    try
    {
      for(auto& [pending_name, pending_fields] : types)
      {
        const auto [entry, inserted] = _p->schema.custom_types.try_emplace(pending_name);
        if(inserted)
        {
          added_types.push_back(entry);
          entry->second = std::move(pending_fields);
        }
      }
      indexed = _p->registered_values.emplace(name, index).first;
      if(custom_schema)
      {
        const auto [entry, inserted] = _p->schema.custom_schemas.emplace(
            type_info->typeName(), std::move(*custom_schema));
        if(inserted)
        {
          added_schema = entry;
        }
      }
      _p->series.emplace_back(std::move(instance));
      _p->schema.fields.emplace_back(std::move(field));
      appended = true;
      const uint64_t hash = ComputeSchemaHash(_p->schema);
      const uint32_t generation = _p->shared->addSeries();
      _p->schema.hash = hash;
      _p->invalidateAnnouncements();
      return RegistrationID(uint32_t(index), generation);
    }
    catch(...)
    {
      if(appended)
      {
        _p->schema.fields.pop_back();
        _p->series.pop_back();
      }
      if(added_schema)
      {
        _p->schema.custom_schemas.erase(*added_schema);
      }
      if(indexed)
      {
        _p->registered_values.erase(*indexed);
      }
      for(const auto& entry : added_types)
      {
        _p->schema.custom_types.erase(entry);
      }
      throw;
    }
  }

  const size_t index = it->second;
  auto& instance = _p->series[index];
  if(_p->shared->isRegistered(index))
  {
    throw std::runtime_error(ChannelPrefix(_p->channel_name) + "value '" + name +
                             "' registered twice (unregister() it first)");
  }
  if(instance.holder != value_ptr)
  {
    throw std::runtime_error(ChannelPrefix(_p->channel_name) + "value '" + name +
                             "' was previously registered with a different type");
  }
  if(!_p->shared->canReregister(index))
  {
    throw std::length_error(ChannelPrefix(_p->channel_name) +
                            "registration slot exhausted: '" + name +
                            "' was re-registered 16 million times");
  }
  instance.holder = std::move(value_ptr);
  // Publish the fully initialized replacement before a mask may enable it.
  // The new generation makes every handle to the previous registration stale.
  return RegistrationID(uint32_t(index), _p->shared->setReregistered(index));
}

LogChannel::LogChannel(std::string name) : _p(new Pimpl)
{
  if(const char* problem = NameProblem(name, true))
  {
    throw std::runtime_error("invalid channel name '" + Printable(name) +
                             "': " + problem);
  }
  _p->schema.channel_name = name;
  _p->channel_name = std::move(name);
  _p->schema.hash = ComputeSchemaHash(_p->schema);
}

std::shared_ptr<LogChannel> LogChannel::create(std::string name)
{
  return std::shared_ptr<LogChannel>(new LogChannel(std::move(name)));
}

const std::string& LogChannel::channelName() const
{
  return _p->channel_name;
}
LogChannel::~LogChannel()
{
  // Calls using the channel object must already be externally synchronized.
  // Links are released without the lock: a dying worker runs sink callbacks.
  std::array<std::unique_ptr<Pimpl::SinkLink>, Pimpl::kMaxSinks> links;
  {
    std::lock_guard const lock(_p->control_mutex);
    for(auto& link : _p->published_sinks)
    {
      link.store(nullptr, std::memory_order_seq_cst);
    }
    _p->waitQuiescent();
    links = std::move(_p->sinks);
  }
}

void LogChannel::setEnabled(const RegistrationID& id, bool enable)
{
  if(!trySetEnabled(id, enable))
  {
    throw std::invalid_argument("setEnabled: stale or invalid RegistrationID");
  }
}

bool LogChannel::trySetEnabled(const RegistrationID& id, bool enable) noexcept
{
  return _p->shared->setEnabled(id, enable);
}

bool LogChannel::isEnabled(const RegistrationID& id) const noexcept
{
  return _p->shared->isEnabled(id);
}

void LogChannel::unregister(const RegistrationID& id)
{
  std::lock_guard const lock(_p->control_mutex);
  if(!_p->shared->isCurrent(id))
  {
    throw std::invalid_argument("unregister: stale or invalid RegistrationID");
  }
  _p->shared->setUnregistered(id.index_);
  _p->waitQuiescent();
  _p->series[id.index_].holder.detach();
}

bool LogChannel::addDataSink(std::shared_ptr<SinkWorker> sink)
{
  if(!sink)
  {
    throw std::invalid_argument("Can't add a null sink");
  }
  std::unique_lock lock(_p->control_mutex);
  // One announcement per worker: a concurrent addDataSink() of the same worker
  // waits for it, then finds the worker attached.
  _p->announced.wait(lock, [&] { return !_p->isAnnouncing(sink.get()); });
  // Duplicate check and free slot; repeated after an unlocked announcement.
  const auto find_slot = [&]() -> std::optional<size_t> {
    if(_p->findLink(sink) != Pimpl::kMaxSinks)
    {
      return std::nullopt;  // already attached
    }
    const auto free_slot = std::find(_p->sinks.rbegin(), _p->sinks.rend(), nullptr);
    if(free_slot == _p->sinks.rend())
    {
      throw std::runtime_error("A channel supports at most eight sinks");
    }
    return size_t(_p->sinks.rend() - free_slot) - 1;  // the highest free slot
  };
  auto free_slot = find_slot();
  if(!free_slot)
  {
    return false;
  }
  auto link = std::make_unique<Pimpl::SinkLink>(sink);
  if(_p->schema_frozen)
  {
    _p->announcing.push_back(sink.get());
    // Ends the announcement on every path, with control_mutex held.
    struct Announcing
    {
      ~Announcing()
      {
        if(!lock.owns_lock())
        {
          lock.lock();
        }
        p.announcing.erase(std::find(p.announcing.begin(), p.announcing.end(), worker));
        p.announced.notify_all();
      }
      Pimpl& p;
      std::unique_lock<std::mutex>& lock;
      const SinkWorker* worker;
    } announcing{ *_p, lock, sink.get() };
    // Same protocol as startLogging(): the sink hears a copy of the final schema
    // with the control mutex released, so it may query the channel. A failing
    // startLogging() can reopen the schema meanwhile and a registration change it:
    // the sink hears it again once it is frozen, else startLogging() announces it.
    do
    {
      const Schema schema = _p->schema;
      lock.unlock();
      sink->addSchema(schema);
      lock.lock();
      link->schema_registered = _p->schema.hash == schema.hash;
    } while(!link->schema_registered && _p->schema_frozen);
    free_slot = find_slot();
    if(!free_slot)
    {
      return false;  // attached concurrently by someone else
    }
  }
  _p->sinks[*free_slot] = std::move(link);
  // Before logging starts, startLogging() gives the link its queue and publishes it.
  if(_p->logging_started.load(std::memory_order_relaxed))
  {
    try
    {
      _p->publish(*free_slot);
    }
    catch(...)
    {
      _p->sinks[*free_slot].reset();  // no queue: not attached
      throw;
    }
  }
  return true;
}

void LogChannel::removeDataSink(std::shared_ptr<SinkWorker> sink)
{
  // The link (and with it, possibly the last reference to the worker, whose
  // destructor drains and runs sink callbacks) dies after the lock is released:
  // callbacks may query the channel.
  std::unique_ptr<Pimpl::SinkLink> removed;
  {
    std::lock_guard const lock(_p->control_mutex);
    const size_t i = _p->findLink(sink);
    if(i == Pimpl::kMaxSinks)
    {
      return;
    }
    _p->published_sinks[i].store(nullptr, std::memory_order_seq_cst);
    _p->waitQuiescent();
    removed = std::move(_p->sinks[i]);
    // Detach before unlocking: an addDataSink() of the same worker on another
    // thread then attaches its new queue after this detach, and the worker
    // delivers the old queue first (per-channel order).
    removed->dropQueue();
  }
}

size_t LogChannel::getNumberOfSinks() const
{
  std::lock_guard const lock(_p->control_mutex);
  size_t count = 0;
  _p->forEachLink([&](const Pimpl::SinkLink&) { ++count; });
  return count;
}

std::vector<std::shared_ptr<SinkWorker>> LogChannel::dataSinks() const
{
  std::lock_guard const lock(_p->control_mutex);
  std::vector<std::shared_ptr<SinkWorker>> sinks;
  _p->forEachLink([&](const Pimpl::SinkLink& link) { sinks.push_back(link.sink); });
  return sinks;
}

Schema LogChannel::getSchema() const
{
  std::lock_guard const lock(_p->control_mutex);
  return _p->schema;
}

WriteTransaction LogChannel::scopedWrite()
{
  return WriteTransaction(*_p->shared);
}

uint64_t LogChannel::writeLockContended() const
{
  return _p->write_lock_contended.load(std::memory_order_relaxed);
}

uint64_t LogChannel::writeLockWaitMaxNs() const
{
  return _p->write_lock_wait_max_ns.load(std::memory_order_relaxed);
}

uint64_t LogChannel::poolExhausted() const
{
  // startLogging() creates the pool before it sets logging_started and never replaces
  // it afterwards; before that no snapshot can have found it exhausted.
  if(!_p->logging_started.load(std::memory_order_acquire))
  {
    return 0;
  }
  return _p->pool->exhausted();
}

void LogChannel::setPayloadCapacity(size_t bytes)
{
  std::lock_guard const lock(_p->control_mutex);
  if(_p->schema_frozen)
  {
    throw std::runtime_error("Payload capacity is frozen");
  }
  if(bytes > kMaxPayloadBytes)
  {
    throw std::length_error("Snapshot payload capacity is too large");
  }
  _p->payload_capacity = bytes;
}

void LogChannel::setPoolCapacity(size_t count)
{
  std::lock_guard const lock(_p->control_mutex);
  if(_p->schema_frozen)
  {
    throw std::runtime_error("Pool capacity is frozen");
  }
  if(count == 0)
  {
    throw std::invalid_argument("Pool capacity must be positive");
  }
  if(count > kMaxPoolSlots)
  {
    throw std::length_error("Snapshot pool capacity is too large");
  }
  _p->pool_capacity = count;
}

void LogChannel::setPoolCapacity(std::chrono::nanoseconds stall_tolerance,
                                 std::chrono::nanoseconds snapshot_period)
{
  // Arguments first: they throw before the frozen check of the count overload.
  const ChannelDefaults in_time{ .pool_stall_tolerance = stall_tolerance,
                                 .pool_snapshot_period = snapshot_period };
  setPoolCapacity(in_time.resolve().pool_slots);
}

// Defined here, next to the setters whose bounds it checks.
ChannelDefaults::Sizes ChannelDefaults::resolve() const
{
  if(payload_capacity > kMaxPayloadBytes)
  {
    throw std::length_error("Snapshot payload capacity is too large");
  }
  Sizes sizes{ pool_capacity, payload_capacity };
  if(pool_stall_tolerance.count() != 0 || pool_snapshot_period.count() != 0)
  {
    if(pool_capacity != 0)
    {
      throw std::invalid_argument("Pool capacity: set it as a count or in time, not "
                                  "both");
    }
    if(pool_stall_tolerance.count() <= 0 || pool_snapshot_period.count() <= 0)
    {
      throw std::invalid_argument("Pool capacity: stall tolerance and snapshot period "
                                  "must both be positive");
    }
    // ceil(stall / period) without the overflow of (stall + period - 1) / period;
    // the quotient is at most INT64_MAX, so adding one cannot wrap.
    const auto whole =
        uint64_t(pool_stall_tolerance.count() / pool_snapshot_period.count());
    const bool remainder =
        pool_stall_tolerance.count() % pool_snapshot_period.count() != 0;
    const uint64_t slots = whole + (remainder ? 1 : 0);
    if(slots > kMaxPoolSlots)
    {
      throw std::length_error("Snapshot pool capacity is too large");
    }
    sizes.pool_slots = size_t(slots);
  }
  else if(pool_capacity > kMaxPoolSlots)
  {
    throw std::length_error("Snapshot pool capacity is too large");
  }
  return sizes;
}

uint64_t LogChannel::payloadReallocations() const
{
  return _p->payload_reallocations.load(std::memory_order_relaxed);
}

uint64_t LogChannel::droppedOversize() const
{
  return _p->dropped_oversize.load(std::memory_order_relaxed);
}

uint64_t LogChannel::snapshotAttempts() const
{
  return _p->attempts.load(std::memory_order_relaxed);
}

uint64_t LogChannel::snapshotsAccepted() const
{
  return _p->accepted.load(std::memory_order_acquire);
}

size_t LogChannel::sinkDropped(const SinkWorker** sinks, uint64_t* dropped,
                               size_t capacity) const
{
  std::lock_guard const lock(_p->control_mutex);
  size_t count = 0;
  _p->forEachLink([&](const Pimpl::SinkLink& link) {
    if(count < capacity)
    {
      sinks[count] = link.sink.get();
      dropped[count] = link.dropped.load(std::memory_order_relaxed);
    }
    ++count;
  });
  return count;
}

uint64_t LogChannel::droppedSnapshots(const std::shared_ptr<SinkWorker>& sink) const
{
  std::lock_guard const lock(_p->control_mutex);
  const size_t i = _p->findLink(sink);
  return i == Pimpl::kMaxSinks ? 0 :
                                 _p->sinks[i]->dropped.load(std::memory_order_relaxed);
}

std::shared_ptr<ChannelSharedState> LogChannel::sharedState() const
{
  return _p->shared;
}

void LogChannel::addCustomType(const std::string& custom_type_name,
                               const FieldsVector& fields)
{
  _p->schema.custom_types[custom_type_name] = fields;
  _p->schema.hash = ComputeSchemaHash(_p->schema);
  _p->invalidateAnnouncements();
}

void LogChannel::checkValueName(const std::string& name) const
{
  CheckSchemaName(_p->channel_name, "value name", name);
}

TypesRegistry& LogChannel::typeRegistry()
{
  return _p->type_registry;
}

std::mutex& LogChannel::controlMutex()
{
  return _p->control_mutex;
}
bool LogChannel::schemaFrozen() const
{
  return _p->schema_frozen;
}
bool LogChannel::hasCustomType(const std::string& type_name) const
{
  return _p->schema.custom_types.contains(type_name);
}

bool LogChannel::isLoggingStarted() const
{
  return _p->logging_started.load(std::memory_order_acquire);
}

void LogChannel::startLogging()
{
  if(_p->logging_started.load(std::memory_order_acquire))
  {
    return;
  }
  // Lock order: the write mutex, then start_mutex, then control_mutex, like a writer
  // that queries the channel inside its transaction. A transaction nests in the
  // caller's own, so startLogging() works inside scopedWrite().
  std::optional<WriteTransaction> transaction(std::in_place, *_p->shared);
  std::lock_guard const serialize(_p->start_mutex);
  if(_p->logging_started.load(std::memory_order_relaxed))
  {
    return;
  }
  std::unique_lock lock(_p->control_mutex);
  // Frozen while sinks are announced so that the schema they receive is final;
  // any failure below reopens it and drops the pool, so a retry starts clean.
  const bool was_frozen = _p->schema_frozen;
  _p->schema_frozen = true;
  try
  {
    if(!_p->pool)
    {
      _p->active_mask.resize(_p->series.size() / 8 + (_p->series.size() % 8 != 0));
      _p->shared->mask_dirty.exchange(false, std::memory_order_seq_cst);
      _p->rebuildMask();
      const auto capacity = _p->initialPayloadCapacity();
      // Publish the pool only after every slot has reserved successfully.
      _p->pool = std::make_shared<SnapshotPool>(_p->pool_capacity, capacity,
                                                _p->active_mask.size());
    }
    transaction.reset();  // writers proceed while the sinks hear the schema
    // Announce to one sink at a time with the control mutex released: a sink
    // may query the channel from onSchema(), and other control operations
    // (including a concurrent addDataSink) proceed meanwhile.
    for(size_t i = 0; i < _p->sinks.size(); ++i)
    {
      if(!_p->sinks[i] || _p->sinks[i]->schema_registered)
      {
        continue;
      }
      auto sink = _p->sinks[i]->sink;
      const Schema schema = _p->schema;
      lock.unlock();
      sink->addSchema(schema);
      lock.lock();
      if(_p->sinks[i] && _p->sinks[i]->sink == sink)
      {
        _p->sinks[i]->schema_registered = true;
      }
    }
  }
  catch(...)
  {
    if(!lock.owns_lock())
    {
      lock.lock();
    }
    _p->schema_frozen = was_frozen;
    _p->pool.reset();
    throw;
  }
  // Last step, under the lock: a queue for every link (as large as the pool;
  // links attached from now on get theirs in addDataSink()), then every link
  // published. A failed allocation, the only throw left, leaves the schema
  // frozen with its pool and nothing published; startLogging() can be called again.
  _p->forEachLink([&](Pimpl::SinkLink& link) { link.ensureQueue(_p->pool->capacity()); });
  for(size_t i = 0; i < _p->sinks.size(); ++i)
  {
    if(_p->sinks[i])
    {
      _p->publish(i);
    }
  }
  _p->logging_started.store(true, std::memory_order_release);
}

SnapshotResult LogChannel::takeSnapshot(std::chrono::nanoseconds timestamp)
{
  _p->attempts.fetch_add(1, std::memory_order_relaxed);
  if(!_p->logging_started.load(std::memory_order_acquire))
  {
    if(getNumberOfSinks() == 0)
    {
      return SnapshotResult::no_sinks;  // nothing to deliver to: stay open
    }
    startLogging();
  }
  return takeSnapshotImpl(timestamp, false);
}

SnapshotResult LogChannel::tryTakeSnapshot(std::chrono::nanoseconds timestamp)
{
  _p->attempts.fetch_add(1, std::memory_order_relaxed);
  if(!_p->logging_started.load(std::memory_order_acquire))
  {
    return SnapshotResult::not_started;
  }
  return takeSnapshotImpl(timestamp, true);
}

SnapshotResult LogChannel::takeSnapshotImpl(std::chrono::nanoseconds timestamp,
                                            bool real_time)
{
  // Without sinks, return before touching the pool or the write mutex. The links
  // are loaded again inside the epoch.
  if(std::none_of(_p->published_sinks.begin(), _p->published_sinks.end(),
                  [](const auto& link) { return link.load(std::memory_order_relaxed); }))
  {
    return SnapshotResult::no_sinks;
  }

  Pimpl::OwedWakes wakes;  // destroyed after write_lock and guard below
  // The write mutex before the epoch: a control operation waiting for the epoch
  // (it may run inside a transaction) never waits for a snapshot that waits for it.
  auto& write_mutex = _p->shared->write_mutex;
  uint64_t blocked_wait_ns = 0;
  bool blocked = false;
  if(!write_mutex.try_lock())
  {
    // A thread that holds the write mutex (scopedWrite() or a guard) fails try_lock()
    // and would wait for itself. The check, a thread-local lookup, runs only here.
    if(_p->shared->inTransactionOnThisThread())
    {
      _p->write_lock_contended.fetch_add(1, std::memory_order_relaxed);
      return SnapshotResult::blocked;
    }
    if(real_time)
    {
      if(!write_mutex.tryLockWithSpin(WriteMutex::kLockSpinNs))
      {
        _p->write_lock_contended.fetch_add(1, std::memory_order_relaxed);
        return SnapshotResult::blocked;
      }
    }
    else
    {
      blocked = write_mutex.lockWithSpin(WriteMutex::kLockSpinNs, &blocked_wait_ns);
    }
  }
  // Held until the last push: producers, when several threads take snapshots, are
  // serialized over the pool scan, the active mask and the single-producer queues.
  std::lock_guard<WriteMutex> write_lock(write_mutex, std::adopt_lock);
  if(blocked)
  {
    _p->write_lock_contended.fetch_add(1, std::memory_order_relaxed);
    auto previous = _p->write_lock_wait_max_ns.load(std::memory_order_relaxed);
    while(previous < blocked_wait_ns &&
          !_p->write_lock_wait_max_ns.compare_exchange_weak(previous, blocked_wait_ns,
                                                            std::memory_order_relaxed))
    {
    }
  }
  Pimpl::EpochGuard guard(*_p);

  std::array<Pimpl::SinkLink*, Pimpl::kMaxSinks> links{};
  size_t last = links.size();  // slot of the last published link
  for(size_t i = 0; i < links.size(); ++i)
  {
    links[i] = _p->published_sinks[i].load(std::memory_order_seq_cst);
    if(links[i])
    {
      last = i;
    }
  }
  if(last == links.size())
  {
    return SnapshotResult::no_sinks;  // removed while this call waited for the mutex
  }

  auto* slot = _p->pool->tryAcquire();  // counts exhaustion itself
  if(!slot)
  {
    return SnapshotResult::pool_exhausted;
  }
  SnapshotRef parent = SnapshotPool::adopt(_p->pool, slot);
  auto& snapshot = slot->snapshot;

  // Under the write mutex, so enable changes made inside a scopedWrite()
  // transaction are seen together with the values they belong to. A rebuilt
  // active bit acquires registration's initialized holder through its SC flag
  // load; see refreshMaskIfDirty() for the ordering argument.
  _p->refreshMaskIfDirty();
  // The RT path sizes against the slot: it neither throws nor allocates.
  const auto payload_size =
      real_time ? _p->payloadSize(snapshot.payload.capacity()) : _p->payloadSize();
  if(payload_size > snapshot.payload.capacity())
  {
    if(real_time)
    {
      _p->dropped_oversize.fetch_add(1, std::memory_order_relaxed);
      return SnapshotResult::oversize;
    }
    snapshot.payload.reserve(checkedDouble(payload_size));
    _p->payload_reallocations.fetch_add(1, std::memory_order_relaxed);
  }
  snapshot.payload.resize(payload_size);
  SerializeMe::SpanBytes payload_buffer(snapshot.payload);
  for(size_t i = 0; i < _p->series.size(); i++)
  {
    if(GetBit(_p->active_mask, i))
    {
      _p->series[i].holder.serialize(payload_buffer);
    }
  }
  snapshot.payload.resize(snapshot.payload.size() - payload_buffer.size());
  snapshot.active_mask = _p->active_mask;
  snapshot.schema_hash = _p->schema.hash;
  snapshot.timestamp = timestamp;

  // Each link but the last gets a clone; the last takes `parent` itself. A
  // refused push leaves its argument intact, so `parent` then still releases
  // its reference at scope exit, once.
  size_t attached = 0, accepted = 0;
  for(size_t i = 0; i <= last; ++i)
  {
    auto* link = links[i];
    if(!link)
    {
      continue;
    }
    ++attached;
    const auto push = i == last ? link->sink->tryPush(*link->queue, std::move(parent)) :
                                  link->sink->tryPush(*link->queue, parent.clone());
    if(push == SinkWorker::Push::refused)
    {
      link->dropped.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    ++accepted;
    if(push == SinkWorker::Push::wake_owed)
    {
      wakes.workers[wakes.count++] = link->sink->wakeTarget();
    }
  }
  if(accepted == 0)
  {
    return SnapshotResult::rejected;
  }
  // Release: pairs with the acquire in snapshotsAccepted() so that a reader that
  // sees this increment also sees this call's `attempts` increment.
  _p->accepted.fetch_add(1, std::memory_order_release);
  return accepted == attached ? SnapshotResult::ok : SnapshotResult::partial;
}

}  // namespace DataTamer
