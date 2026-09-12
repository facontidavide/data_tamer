#include "data_tamer/channel.hpp"
#include "data_tamer/data_sink.hpp"
#include "data_tamer/contrib/SerializeMe.hpp"
#include "data_tamer/details/shared_state.hpp"
#include "data_tamer/details/snapshot_pool.hpp"
#include "ConcurrentQueue/concurrentqueue.h"

#include <algorithm>
#include <array>
#include <limits>
#include <thread>
#include <unordered_map>

namespace DataTamer
{
namespace
{
size_t checkedDouble(size_t size)
{
  if(size > std::vector<uint8_t>().max_size() / 2)
    throw std::length_error("Snapshot payload is too large to reserve");
  return 2 * size;
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
  mutable std::mutex control_mutex;
  std::vector<ValueHolder> series;
  std::unordered_map<std::string, size_t> registered_values;
  std::shared_ptr<ChannelSharedState> shared = std::make_shared<ChannelSharedState>();
  std::atomic<uint64_t> write_lock_contended{ 0 };
  std::atomic<uint64_t> write_lock_wait_max_ns{ 0 };
  ActiveMask active_mask;  // Snapshot-thread-owned, independent of retained slots.
  size_t payload_capacity = 0;
  size_t pool_capacity = SnapshotPool::kDefaultCapacity;
  std::atomic<bool> strict_mode{ false };
  std::atomic<uint64_t> payload_reallocations{ 0 };
  std::atomic<uint64_t> dropped_oversize{ 0 };
  std::shared_ptr<SnapshotPool> pool;
  Schema schema;
  bool schema_frozen = false;    // control_mutex held
  bool logging_started = false;  // snapshot thread, or control_mutex held

  void rebuildMask()
  {
    std::fill(active_mask.begin(), active_mask.end(), 0xFF);
    for(size_t i = 0; i < series.size(); ++i)
      if(!shared->isEnabled(i))
        SetBit(active_mask, i, false);
  }

  // Reader side of the mask handshake. Both operations are SC, so a reader that
  // saw the flag clear is ordered before any later dirty store and is covered by
  // waitQuiescent(); the plain load keeps the common path free of a locked RMW.
  void refreshMaskIfDirty()
  {
    if(shared->mask_dirty.load(std::memory_order_seq_cst) &&
       shared->mask_dirty.exchange(false, std::memory_order_seq_cst))
      rebuildMask();
  }

  // Caller holds write_mutex; size and serialization use this same cached mask.
  size_t payloadSize() const
  {
    size_t size = 0;
    const auto limit = std::vector<uint8_t>().max_size();
    for(size_t i = 0; i < series.size(); ++i)
    {
      if(GetBit(active_mask, i))
      {
        const auto field_size = series[i].holder.getSerializedSize();
        if(field_size > limit - size)
          throw std::length_error("Snapshot payload is too large");
        size += field_size;
      }
    }
    return size;
  }

  struct SinkLink
  {
    // Members are destroyed in reverse order: token must die before its sink.
    std::shared_ptr<DataSinkBase> sink;
    std::unique_ptr<moodycamel::ProducerToken> token;
    std::atomic<uint64_t> dropped{ 0 };
    bool schema_registered = false;
  };
  static constexpr size_t kMaxSinks = 8;
  std::array<std::unique_ptr<SinkLink>, kMaxSinks> sinks;
  std::array<std::atomic<SinkLink*>, kMaxSinks> published_sinks{};
  std::atomic<uint64_t> epoch{ 0 };

  // Controller holds control_mutex. SC order prevents a reader from both
  // seeing an old link/cached mask and being missed by this epoch observation:
  // its entry precedes its old-pointer load/dirty exchange, which precedes
  // removal's SC publication and this load. Wait only for that reader; later
  // readers see the new publication. Exit is release, observation is acquire.
  void waitQuiescent()
  {
    const auto observed = epoch.load(std::memory_order_seq_cst);
    while((observed & 1) && epoch.load(std::memory_order_seq_cst) == observed)
      std::this_thread::yield();
  }

  // Reader side of waitQuiescent(): odd epoch while a snapshot is in progress.
  struct EpochGuard
  {
    std::atomic<uint64_t>& epoch;
    explicit EpochGuard(std::atomic<uint64_t>& value) : epoch(value)
    {
      epoch.fetch_add(1, std::memory_order_seq_cst);
    }
    ~EpochGuard() { epoch.fetch_add(1, std::memory_order_seq_cst); }
  };
};

RegistrationID LogChannel::registerValueImpl(const std::string& name,
                                             ValuePtr&& value_ptr,
                                             CustomSerializer::Ptr type_info)
{
  // The public registration template holds control_mutex, including type discovery.
  if(name.find(' ') != std::string::npos)
    throw std::runtime_error("name can not contain spaces");

  auto it = _p->registered_values.find(name);
  if(it == _p->registered_values.end())
  {
    if(_p->schema_frozen)
      throw std::runtime_error("Can't register a new value once recording started");
    const auto type = value_ptr.type();
    const std::string type_name = type_info ? type_info->typeName() : ToStr(type);
    TypeField field{ name, type, type_name, value_ptr.isVector(),
                     value_ptr.vectorSize() };
    Pimpl::ValueHolder instance;
    instance.name = name;
    instance.holder = std::move(value_ptr);
    _p->series.emplace_back(std::move(instance));
    _p->shared->addSeries();
    const size_t index = _p->series.size() - 1;
    _p->registered_values.insert({ name, index });

    _p->schema.fields.emplace_back(std::move(field));
    if(type_info)
    {
      auto custom_schema = type_info->typeSchema();
      if(custom_schema && _p->schema.custom_types.count(type_info->typeName()) == 0)
        _p->schema.custom_schemas.insert({ type_info->typeName(), *custom_schema });
    }

    _p->schema.hash = ComputeSchemaHash(_p->schema);
    return { index, 1 };
  }

  const size_t index = it->second;
  auto& instance = _p->series[index];
  if(_p->shared->isRegistered(index))
    throw std::runtime_error("This name was already registered. Unregister it first");
  if(instance.holder != value_ptr)
    throw std::runtime_error("Can't change the type of a previously registered value");
  instance.holder = std::move(value_ptr);
  // Publish the fully initialized replacement before a mask may enable it.
  _p->shared->setRegistered(index, true);
  return { index, 1 };
}

LogChannel::LogChannel(std::string name) : _p(new Pimpl)
{
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
  std::lock_guard const lock(_p->control_mutex);
  for(auto& link : _p->published_sinks)
    link.store(nullptr, std::memory_order_seq_cst);
  _p->waitQuiescent();
  for(auto& link : _p->sinks)
    link.reset();
}

void LogChannel::setEnabled(const RegistrationID& id, bool enable)
{
  _p->shared->setEnabled(id, enable);
}

void LogChannel::unregister(const RegistrationID& id)
{
  std::lock_guard const lock(_p->control_mutex);
  _p->shared->setRegistered(id, false);
  _p->waitQuiescent();
  for(size_t i = 0; i < id.fields_count; i++)
    _p->series[id.first_index + i].holder.detach();
}

void LogChannel::addDataSink(std::shared_ptr<DataSinkBase> sink)
{
  if(!sink)
    throw std::invalid_argument("Can't add a null sink");
  std::lock_guard const lock(_p->control_mutex);
  size_t free_slot = Pimpl::kMaxSinks;
  for(size_t i = 0; i < _p->sinks.size(); ++i)
  {
    if(_p->sinks[i] && _p->sinks[i]->sink == sink)
      return;
    if(!_p->sinks[i])
      free_slot = i;
  }
  if(free_slot == Pimpl::kMaxSinks)
    throw std::runtime_error("A channel supports at most eight sinks");
  auto link = std::make_unique<Pimpl::SinkLink>();
  link->sink = std::move(sink);
  link->token = link->sink->makeProducerToken();
  if(_p->schema_frozen)
  {
    link->sink->addChannel(_p->channel_name, _p->schema);
    link->schema_registered = true;
  }
  // Neither a throwing token allocation nor addChannel can publish a partial link.
  _p->sinks[free_slot] = std::move(link);
  if(_p->logging_started)
    _p->published_sinks[free_slot].store(_p->sinks[free_slot].get(),
                                         std::memory_order_seq_cst);
}

void LogChannel::removeDataSink(std::shared_ptr<DataSinkBase> sink)
{
  std::lock_guard const lock(_p->control_mutex);
  for(size_t i = 0; i < _p->sinks.size(); ++i)
  {
    if(_p->sinks[i] && _p->sinks[i]->sink == sink)
    {
      _p->published_sinks[i].store(nullptr, std::memory_order_seq_cst);
      _p->waitQuiescent();
      _p->sinks[i].reset();
      return;
    }
  }
}

size_t LogChannel::getNumberOfSinks() const
{
  std::lock_guard const lock(_p->control_mutex);
  size_t count = 0;
  for(const auto& link : _p->sinks)
    count += bool(link);
  return count;
}

Schema LogChannel::getSchema() const
{
  std::lock_guard const lock(_p->control_mutex);
  return _p->schema;
}

Mutex& LogChannel::writeMutex()
{
  return _p->shared->write_mutex;
}

ChannelSharedState::Transaction LogChannel::scopedWrite()
{
  return ChannelSharedState::Transaction(*_p->shared);
}

uint64_t LogChannel::writeLockContended() const
{
  return _p->write_lock_contended.load(std::memory_order_relaxed);
}

uint64_t LogChannel::writeLockWaitMaxNs() const
{
  return _p->write_lock_wait_max_ns.load(std::memory_order_relaxed);
}

LogChannel::Stats LogChannel::stats() const
{
  return { writeLockContended(), writeLockWaitMaxNs(), poolExhausted(),
           payloadReallocations(), droppedOversize() };
}

uint64_t LogChannel::poolExhausted() const
{
  std::lock_guard const lock(_p->control_mutex);  // pool is created under it
  return _p->pool ? _p->pool->exhausted() : 0;
}

void LogChannel::setPayloadCapacity(size_t bytes)
{
  std::lock_guard const lock(_p->control_mutex);
  if(_p->schema_frozen)
    throw std::runtime_error("Payload capacity is frozen");
  if(bytes > std::vector<uint8_t>().max_size())
    throw std::length_error("Snapshot payload capacity is too large");
  _p->payload_capacity = bytes;
}

void LogChannel::setPoolCapacity(size_t count)
{
  std::lock_guard const lock(_p->control_mutex);
  if(_p->schema_frozen)
    throw std::runtime_error("Pool capacity is frozen");
  if(count == 0)
    throw std::invalid_argument("Pool capacity must be positive");
  if(count > std::numeric_limits<std::ptrdiff_t>::max() / sizeof(PoolSlot))
    throw std::length_error("Snapshot pool capacity is too large");
  _p->pool_capacity = count;
}

void LogChannel::setStrictMode(bool strict)
{
  _p->strict_mode.store(strict, std::memory_order_relaxed);
}

uint64_t LogChannel::payloadReallocations() const
{
  return _p->payload_reallocations.load(std::memory_order_relaxed);
}

uint64_t LogChannel::droppedOversize() const
{
  return _p->dropped_oversize.load(std::memory_order_relaxed);
}

uint64_t LogChannel::droppedSnapshots(const std::shared_ptr<DataSinkBase>& sink) const
{
  std::lock_guard const lock(_p->control_mutex);
  for(const auto& link : _p->sinks)
    if(link && link->sink == sink)
      return link->dropped.load(std::memory_order_relaxed);
  return 0;
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
  return _p->schema.custom_types.count(type_name) != 0;
}

const ActiveMask& LogChannel::getActiveFlags()
{
  return _p->active_mask;
}

bool LogChannel::takeSnapshot(std::chrono::nanoseconds timestamp)
{
  // First call freezes even without sinks. Failed preparation is retryable;
  // successful addChannel calls are remembered and nothing is published early.
  if(!_p->logging_started)
  {
    std::lock_guard const lock(_p->control_mutex);
    _p->schema_frozen = true;
    if(!_p->pool)
    {
      std::lock_guard<WriteMutex> write_lock(_p->shared->write_mutex);
      _p->active_mask.resize(_p->series.size() / 8 + (_p->series.size() % 8 != 0));
      _p->shared->mask_dirty.exchange(false, std::memory_order_seq_cst);
      _p->rebuildMask();
      const auto capacity = std::max(
          { _p->payload_capacity, checkedDouble(_p->payloadSize()), size_t(256) });
      // Publish the pool only after every slot has reserved successfully.
      _p->pool = std::make_shared<SnapshotPool>(_p->pool_capacity, capacity,
                                                _p->active_mask.size(), _p->channel_name);
    }
    for(auto& link : _p->sinks)
    {
      if(link && !link->schema_registered)
      {
        link->sink->addChannel(_p->channel_name, _p->schema);
        link->schema_registered = true;
      }
    }
    for(size_t i = 0; i < _p->sinks.size(); ++i)
      _p->published_sinks[i].store(_p->sinks[i].get(), std::memory_order_seq_cst);
    _p->logging_started = true;
  }

  Pimpl::EpochGuard guard(_p->epoch);

  std::array<Pimpl::SinkLink*, Pimpl::kMaxSinks> links{};
  bool has_sinks = false;
  for(size_t i = 0; i < links.size(); ++i)
  {
    links[i] = _p->published_sinks[i].load(std::memory_order_seq_cst);
    has_sinks |= links[i] != nullptr;
  }
  if(!has_sinks)
    return false;

  auto* slot = _p->pool->tryAcquire();  // counts exhaustion itself
  if(!slot)
    return false;
  SnapshotRef parent(_p->pool, slot);
  auto& snapshot = slot->snapshot;

  // A rebuilt active bit acquires registration's initialized holder through
  // its SC flag load; see refreshMaskIfDirty() for the ordering argument.
  _p->refreshMaskIfDirty();

  {
    auto& write_mutex = _p->shared->write_mutex;
    uint64_t blocked_wait_ns = 0;
    const bool blocked =
        write_mutex.lockWithSpin(WriteMutex::kLockSpinNs, &blocked_wait_ns);
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
    const auto payload_size = _p->payloadSize();
    if(payload_size > snapshot.payload.capacity())
    {
      if(_p->strict_mode.load(std::memory_order_relaxed))
      {
        _p->dropped_oversize.fetch_add(1, std::memory_order_relaxed);
        return false;
      }
      snapshot.payload.reserve(checkedDouble(payload_size));
      _p->payload_reallocations.fetch_add(1, std::memory_order_relaxed);
    }
    snapshot.payload.resize(payload_size);
    SerializeMe::SpanBytes payload_buffer(snapshot.payload);
    for(size_t i = 0; i < _p->series.size(); i++)
      if(GetBit(_p->active_mask, i))
        _p->series[i].holder.serialize(payload_buffer);
    snapshot.payload.resize(snapshot.payload.size() - payload_buffer.size());
  }

  snapshot.active_mask = _p->active_mask;
  snapshot.schema_hash = _p->schema.hash;
  snapshot.timestamp = timestamp;

  bool all_pushed = true;
  for(auto* link : links)
  {
    if(link && !link->sink->tryPush(*link->token, parent.clone()))
    {
      link->dropped.fetch_add(1, std::memory_order_relaxed);
      all_pushed = false;
    }
  }
  return all_pushed;
}

}  // namespace DataTamer
