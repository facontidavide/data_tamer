#pragma once

#include "data_tamer/data_sink.hpp"
#include "data_tamer/sinks/mcap_sink.hpp"  // details::NumberedPath

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace DataTamer
{

/// Configuration of an MCAPRingSink.
struct MCAPRingOptions
{
  /// Dump number N (1, 2, 3, ...) is written to details::NumberedPath(filepath, N):
  /// "crash.mcap" -> "crash_1.mcap", "crash_2.mcap", ... Numbers whose file
  /// exists already (dumps of an earlier run, for instance) are skipped, so
  /// no file is ever overwritten.
  std::string filepath = "flight_recorder.mcap";
  /// How much history before the trigger a dump contains, in snapshot time.
  std::chrono::nanoseconds window = std::chrono::seconds(10);
  /// Size of the RAM ring. A second buffer of the same size holds the dump
  /// being written, so the sink uses about 2 * capacity_bytes. When the ring
  /// is full the oldest snapshots are evicted, even if younger than `window`
  /// or part of an active dump (see MCAPRingDump::truncated).
  size_t capacity_bytes = size_t(64) * 1024 * 1024;
  /// Compress the dump files with zstd.
  bool compression = false;
};

/// Outcome of one dump, passed to the MCAPRingSink dump callback.
struct MCAPRingDump
{
  std::string path;
  /// Timestamp of the snapshot that triggered the dump.
  std::chrono::nanoseconds trigger_time{ 0 };
  /// Requested interval: the dump holds the stored snapshots whose timestamp
  /// is in [start, end]. flushPendingDump() may cut `end`.
  std::chrono::nanoseconds start{ 0 };
  std::chrono::nanoseconds end{ 0 };
  /// Timestamps of the oldest and newest message written (0 if none).
  std::chrono::nanoseconds first_message{ 0 };
  std::chrono::nanoseconds last_message{ 0 };
  size_t messages = 0;
  /// The ring was full and evicted snapshots of [start, end] before the
  /// dump was written: the file starts later than `start`, or misses part or
  /// all of the interval when the dump waited long for a busy writer. Raise
  /// capacity_bytes. (Evictions before the trigger are tracked by their newest
  /// timestamp; after snapshot time jumps backwards, e.g. a simulation reset,
  /// only evictions stamped within [start, end] count.)
  bool truncated = false;
  /// False if the file could not be opened or written (disk full, I/O error):
  /// `error` says why. A partially written file is left at `path`.
  bool ok = false;
  std::string error;
};

/// Counters and ring content of an MCAPRingSink, returned by
/// MCAPRingSink::stats(). Each field also has its own getter on the sink.
struct MCAPRingStats
{
  uint64_t dumps_written = 0;
  uint64_t dumps_failed = 0;
  /// Dumps that were complete but found the writer busy with the previous one,
  /// and were handed over at a later snapshot (counted once per dump).
  uint64_t writer_busy_retries = 0;
  /// Snapshots evicted from the ring because it was full (not because of age).
  uint64_t evicted_by_capacity = 0;
  /// Snapshots larger than the whole ring, never stored.
  uint64_t dropped_oversize = 0;
  /// Current content of the ring.
  size_t stored_snapshots = 0;
  size_t stored_bytes = 0;
  /// Snapshot timestamps of the first and the last record in the ring, in
  /// delivery order: the next one to be evicted and the one stored last. Their
  /// difference is the history the ring holds now. Age eviction keeps it at
  /// most about `window` (more while a dump is active); if it stays below
  /// `window` while evicted_by_capacity grows, capacity_bytes is too small.
  /// Channels are delivered separately, so records of other channels may be
  /// older or newer by the delivery skew, and newest < oldest after snapshot
  /// time jumps backwards (simulation reset). Both are 0 when stored_snapshots
  /// is 0; 0 is also a valid timestamp, so test stored_snapshots.
  std::chrono::nanoseconds oldest_timestamp{ 0 };
  std::chrono::nanoseconds newest_timestamp{ 0 };
};

/**
 * @brief Flight recorder: keeps the last `window` of every channel attached to
 * it in a preallocated RAM ring and writes an MCAP file only when asked to,
 * with requestDump(), for example on a protective stop or a fault.
 *
 *   MCAPRingOptions options;
 *   options.filepath = "fault.mcap";
 *   options.window = std::chrono::seconds(5);
 *   auto worker = MCAPRingSink::create(options);
 *   channel->addDataSink(worker);
 *   auto& recorder = worker->as<MCAPRingSink>();
 *   ...
 *   recorder.requestDump(std::chrono::seconds(2));  // from any thread, RT-safe
 *
 * Trigger and content. The trigger is the first snapshot delivered to the sink
 * after the request, and all times are snapshot timestamps, so a dump behaves
 * the same in simulation, replay and on hardware. With trigger time T, the
 * dump contains the stored snapshots of every channel with a timestamp in
 * [T - window, T + post_trigger], and the MCAP schema and channel records of
 * the channels that appear in it. It is complete at the first delivered
 * snapshot with a timestamp > T + post_trigger. Each channel queues its
 * snapshots separately, so delivery is not strictly in timestamp order across
 * channels: a snapshot of another channel stamped <= T + post_trigger but
 * delivered after the one that completed the dump is not included.
 *
 * Real time. onSnapshot() copies mask and payload into the ring and releases
 * the pool slot at once; after the ring is allocated it allocates nothing.
 * A finished dump is copied into the second buffer and written to disk by a
 * writer thread of the sink, so the SinkWorker never waits for file I/O. If
 * the writer is still busy with the previous dump, the hand-off is retried at
 * the next snapshot; meanwhile age eviction spares the dumped interval (capacity
 * eviction does not, see MCAPRingDump::truncated). The hand-off copies the
 * records of [start, end] into the second buffer on the worker thread, a
 * memcpy of up to capacity_bytes.
 *
 * Flush. flushPendingDump() writes the active request at once with what the
 * ring holds, from any thread except the dump callback, whether the worker is
 * running or not; there is no need to stop the worker. At shutdown,
 * SinkWorker::stop() (and the worker's destructor) calls it from onStop() once
 * the last snapshot is delivered, and waits for the writer, so a request made
 * just before shutdown is written. The sink's own destructor finishes the file
 * being written but does not start a pending dump.
 */
class MCAPRingSink : public DataSink
{
public:
  explicit MCAPRingSink(MCAPRingOptions options);
  ~MCAPRingSink() override;
  MCAPRingSink(const MCAPRingSink&) = delete;
  MCAPRingSink& operator=(const MCAPRingSink&) = delete;

  /// Ready-to-attach sink: SinkWorker::create<MCAPRingSink>(options).
  static std::shared_ptr<SinkWorker> create(MCAPRingOptions options)
  {
    return SinkWorker::create<MCAPRingSink>(std::move(options));
  }

  /**
   * @brief Ask for a dump that also covers `post_trigger` after the trigger.
   * Lock-free and allocation-free (a single compare-exchange): call it from any
   * thread, real-time ones included.
   *
   * Returns false, and the request is ignored, while an earlier request is still
   * active: from its requestDump() until its dump has been handed to the writer
   * thread. Requests are not coalesced: an ignored request does not extend the
   * active dump. Writing the file to disk does not block new requests.
   */
  bool requestDump(std::chrono::nanoseconds post_trigger = std::chrono::nanoseconds(0));

  /// True from an accepted requestDump() until its dump is handed to the writer.
  [[nodiscard]] bool dumpRequested() const;

  /**
   * @brief Write the active request, if any, with what the ring holds now,
   * and wait until it is on disk. A request no snapshot has triggered yet uses
   * the newest snapshot timestamp seen as trigger; a dump still waiting for
   * its post-trigger interval is cut at the newest timestamp seen. Snapshots
   * queued but not yet delivered are not in the ring: after SinkWorker::stop()
   * or drain() they are. SinkWorker::stop() calls it after the last delivery.
   *
   * Safe while the SinkWorker delivers snapshots: the worker keeps storing
   * them, and a snapshot that completes the request first hands it off itself.
   * The worker waits for the ring lock while the dump interval is copied into
   * the writer's buffer (the same copy a snapshot-completed dump makes). Never
   * call it from the dump callback (std::logic_error).
   *
   * Returns true if a request was active when it was called (triggered, or
   * pending once the sink has received a snapshot); that dump is then on disk
   * (see MCAPRingDump::ok for the outcome). In every case it returns after the
   * writer has finished all dumps handed to it so far, so a request accepted
   * before the call and already handed off is on disk too.
   */
  bool flushPendingDump();

  /// Wait until the writer thread has no dump in progress.
  void waitForWriter();

  /// Called on the writer thread after each dump, successful or not. It may
  /// call stats(), dumpRequested() and requestDump(); waitForWriter() and
  /// flushPendingDump() would wait for the callback itself and throw
  /// std::logic_error there.
  void setDumpCallback(std::function<void(const MCAPRingDump&)> callback);

  /// All counters at once. Built here, in the header, from the getters below,
  /// so that MCAPRingStats can gain fields without changing the library ABI.
  /// Each field is read separately: while the worker delivers, the fields
  /// may come from slightly different moments.
  [[nodiscard]] MCAPRingStats stats() const
  {
    MCAPRingStats stats;
    stats.dumps_written = dumpsWritten();
    stats.dumps_failed = dumpsFailed();
    stats.writer_busy_retries = writerBusyRetries();
    stats.evicted_by_capacity = evictedByCapacity();
    stats.dropped_oversize = droppedOversize();
    stats.stored_snapshots = storedSnapshots();
    stats.stored_bytes = storedBytes();
    stats.oldest_timestamp = oldestTimestamp();
    stats.newest_timestamp = newestTimestamp();
    return stats;
  }

  /// One getter per MCAPRingStats field, each read under the sink's lock;
  /// see the field for its meaning. Callable from any thread, including the
  /// dump callback; not real-time safe (they take a mutex).
  [[nodiscard]] uint64_t dumpsWritten() const;
  [[nodiscard]] uint64_t dumpsFailed() const;
  [[nodiscard]] uint64_t writerBusyRetries() const;
  [[nodiscard]] uint64_t evictedByCapacity() const;
  [[nodiscard]] uint64_t droppedOversize() const;
  [[nodiscard]] size_t storedSnapshots() const;
  [[nodiscard]] size_t storedBytes() const;
  [[nodiscard]] std::chrono::nanoseconds oldestTimestamp() const;
  [[nodiscard]] std::chrono::nanoseconds newestTimestamp() const;

protected:
  void onSchema(const Schema& schema) override;
  void onSnapshot(const SnapshotRef& snapshot) override;
  /// Writes a pending dump, as flushPendingDump().
  void onStop() override;

private:
  struct Pimpl;
  // Shared with the writer thread, so that it outlives a sink destroyed from
  // its own dump callback.
  std::shared_ptr<Pimpl> _p;
};

}  // namespace DataTamer
