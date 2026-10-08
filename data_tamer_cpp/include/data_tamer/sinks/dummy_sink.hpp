#pragma once

#include "data_tamer/data_sink.hpp"
#include "data_tamer/types.hpp"

#include <mutex>
#include <unordered_map>

namespace DataTamer
{

/**
 * @brief Counts the snapshots it receives and keeps the latest one. For tests.
 *
 * The accessors lock internally, so they can be called from any thread. Call
 * SinkWorker::drain() before asserting, instead of sleeping.
 */
class DummySink : public DataSink
{
public:
  static std::shared_ptr<SinkWorker> create() { return SinkWorker::create<DummySink>(); }

  /// Copy of the latest snapshot delivered.
  Snapshot latestSnapshot() const
  {
    std::scoped_lock lk(mutex_);
    return latest_snapshot_;
  }

  /// Payload size in bytes of the latest snapshot (no copy).
  size_t latestPayloadSize() const
  {
    std::scoped_lock lk(mutex_);
    return latest_snapshot_.payload.size();
  }

  /// Copy of the latest snapshot's active mask (cheaper than latestSnapshot()).
  ActiveMask latestActiveMask() const
  {
    std::scoped_lock lk(mutex_);
    return latest_snapshot_.active_mask;
  }

  /// Number of snapshots delivered for the channel with this schema hash (0 if unknown).
  long snapshotsCount(uint64_t hash) const
  {
    std::scoped_lock lk(mutex_);
    auto it = snapshots_count_.find(hash);
    return it == snapshots_count_.end() ? 0 : it->second;
  }

  size_t schemasCount() const
  {
    std::scoped_lock lk(mutex_);
    return schemas_.size();
  }

  /// Hash of one registered schema, which one is unspecified when there are several.
  /// Precondition: schemasCount() >= 1.
  uint64_t firstSchemaHash() const
  {
    std::scoped_lock lk(mutex_);
    return schemas_.begin()->first;
  }

  Schema schema(uint64_t hash) const
  {
    std::scoped_lock lk(mutex_);
    return schemas_.at(hash);
  }

protected:
  void onSchema(Schema const& schema) override
  {
    std::scoped_lock lk(mutex_);
    schemas_[schema.hash] = schema;
    snapshots_count_[schema.hash] = 0;
  }

  void onSnapshot(const SnapshotRef& snapshot) override
  {
    std::scoped_lock lk(mutex_);
    latest_snapshot_ = *snapshot;
    auto it = snapshots_count_.find(snapshot->schema_hash);
    if(it != snapshots_count_.end())
    {
      it->second++;
    }
  }

private:
  mutable std::mutex mutex_;
  std::unordered_map<uint64_t, Schema> schemas_;
  std::unordered_map<uint64_t, long> snapshots_count_;
  Snapshot latest_snapshot_;
};

}  // namespace DataTamer
