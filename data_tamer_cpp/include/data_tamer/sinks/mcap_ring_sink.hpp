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
  /// dump was written: the file starts later than `start`. Raise capacity_bytes.
  /// (Evictions are tracked by their newest timestamp; after snapshot time
  /// jumps backwards, e.g. a simulation reset, only evictions stamped within
  /// [start, end] count.)
  bool truncated = false;
  /// False if the file could not be opened or written (disk full, I/O error):
  /// `error` says why. A partially written file is left at `path`.
  bool ok = false;
  std::string error;
};

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
 * Shutdown. A request made just before shutdown has no snapshot left to
 * trigger it: call flushPendingDump() after SinkWorker::stop(). The destructor
 * finishes the file being written but does not start a pending dump.
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
   * its post-trigger interval is cut at the newest timestamp seen. Call it after
   * SinkWorker::stop() (or between drain() calls with manual delivery).
   * Never call it from the dump callback (std::logic_error).
   * Returns true if it wrote a dump (see MCAPRingDump::ok for the outcome).
   * In every case it returns after the writer has finished all dumps handed
   * to it so far.
   */
  bool flushPendingDump();

  /// Wait until the writer thread has no dump in progress.
  void waitForWriter();

  /// Called on the writer thread after each dump, successful or not. It may
  /// call stats(), dumpRequested() and requestDump(); waitForWriter() and
  /// flushPendingDump() would wait for the callback itself and throw
  /// std::logic_error there.
  void setDumpCallback(std::function<void(const MCAPRingDump&)> callback);

  [[nodiscard]] MCAPRingStats stats() const;

protected:
  void onSchema(const Schema& schema) override;
  void onSnapshot(const SnapshotRef& snapshot) override;

private:
  struct Pimpl;
  // Shared with the writer thread, so that it outlives a sink destroyed from
  // its own dump callback.
  std::shared_ptr<Pimpl> _p;
};

}  // namespace DataTamer
