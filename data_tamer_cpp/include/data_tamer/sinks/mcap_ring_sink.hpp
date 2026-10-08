#pragma once

#include "data_tamer/data_sink.hpp"
#include "data_tamer/details/abi.hpp"
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
  /// Dump N (1, 2, ...) goes to details::NumberedPath(filepath, N): "crash.mcap" ->
  /// "crash_1.mcap", "crash_2.mcap". Names that exist already are skipped, so no file
  /// is ever overwritten.
  std::string filepath = "flight_recorder.mcap";
  /// History kept before the trigger, in snapshot time.
  std::chrono::nanoseconds window = std::chrono::seconds(10);
  /// Size of the RAM ring. A second buffer of the same size holds the dump being
  /// written, so the sink uses about 2 * capacity_bytes. A full ring evicts its oldest
  /// snapshots even if younger than `window` (see MCAPRingDump::truncated).
  size_t capacity_bytes = size_t(64) * 1024 * 1024;
  /// Compress the dump files with zstd.
  bool compression = false;
};

/// Outcome of one dump, passed to the dump callback.
struct MCAPRingDump
{
  std::string path;
  /// Timestamp of the snapshot that triggered the dump.
  std::chrono::nanoseconds trigger_time{ 0 };
  /// Requested interval [start, end]. flushPendingDump() can cut `end`.
  std::chrono::nanoseconds start{ 0 };
  std::chrono::nanoseconds end{ 0 };
  /// Timestamps of the oldest and newest message written (0 if none).
  std::chrono::nanoseconds first_message{ 0 };
  std::chrono::nanoseconds last_message{ 0 };
  size_t messages = 0;
  /// The ring evicted snapshots of [start, end] before the dump was written, so the file
  /// can start late or miss part of the interval. Raise capacity_bytes.
  bool truncated = false;
  /// False if the file could not be opened or written (disk full, I/O error): `error`
  /// says why, and a partial file can be left at `path`.
  bool ok = false;
  std::string error;
};

/// Counters and ring content, returned by MCAPRingSink::stats().
struct MCAPRingStats
{
  uint64_t dumps_written = 0;
  uint64_t dumps_failed = 0;
  /// Complete dumps that found the writer busy with the previous one and were handed
  /// over at a later snapshot (counted once per dump).
  uint64_t writer_busy_retries = 0;
  /// Snapshots evicted because the ring was full, not because of age.
  uint64_t evicted_by_capacity = 0;
  /// Snapshots larger than the whole ring, never stored.
  uint64_t dropped_oversize = 0;
  /// Current content of the ring.
  size_t stored_snapshots = 0;
  size_t stored_bytes = 0;
  /// Timestamps of the first and last record in the ring, in delivery order (both 0 if
  /// the ring is empty: check stored_snapshots). Their difference is the history held:
  /// if it stays below `window` while evicted_by_capacity grows, capacity_bytes is too
  /// small. With several channels it is off by the delivery skew, and newest < oldest
  /// after snapshot time jumps backwards.
  std::chrono::nanoseconds oldest_timestamp{ 0 };
  std::chrono::nanoseconds newest_timestamp{ 0 };
};

/**
 * @brief Flight recorder: keeps the last `window` of every attached channel in a
 * preallocated RAM ring and writes an MCAP file only on requestDump(), for example
 * on a protective stop or a fault.
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
 * Content. The trigger is the first snapshot delivered after the request, and all times
 * are snapshot timestamps, so a dump behaves the same in simulation, replay and on
 * hardware. With trigger time T, the dump holds the stored snapshots of every channel
 * stamped in [T - window, T + post_trigger]. It is complete at the first delivered
 * snapshot stamped after T + post_trigger: a snapshot of another channel delivered
 * later is not included, even if stamped earlier.
 *
 * Threads. onSnapshot() copies each snapshot into the ring, releases its pool slot at
 * once and allocates nothing. A finished dump is copied (a memcpy of up to capacity_bytes
 * on the worker thread) into a second buffer and written by a writer thread of the sink,
 * so the SinkWorker never waits for file I/O. If that thread is still busy with the
 * previous dump, the hand-off is retried at the next snapshot.
 *
 * Shutdown. SinkWorker::stop() (and the worker's destructor) writes a pending request
 * and waits for the writer, so a request made just before shutdown is not lost. The
 * sink's own destructor finishes the file in progress but starts no pending dump.
 */
class MCAPRingSink : public DataSink
{
public:
  /// Allocates both buffers now. Throws std::invalid_argument if capacity_bytes is 0 or
  /// window is negative.
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
   * Lock-free and allocation-free: callable from any thread, real-time ones included.
   *
   * Returns false, and ignores the request, while an earlier one is active (until its
   * dump is handed to the writer thread). Requests are not merged, and a file being
   * written does not block new ones.
   */
  bool requestDump(std::chrono::nanoseconds post_trigger = std::chrono::nanoseconds(0));

  /// True from an accepted requestDump() until its dump is handed to the writer.
  [[nodiscard]] bool dumpRequested() const;

  /**
   * @brief Write the active request now, with what the ring holds, and wait until it is
   * on disk. A request no snapshot has triggered yet takes the newest timestamp of the
   * current run as its trigger (a new run starts when the snapshot clock of a channel
   * steps back, as in a simulation reset), and a dump still collecting its post-trigger
   * interval is cut at the newest timestamp seen.
   * Queued snapshots that are not delivered yet are not in the ring (SinkWorker::drain()
   * delivers them).
   *
   * Callable from any thread, whether the worker runs or is stopped, but not from the
   * dump callback (std::logic_error). Delivery waits while the dump interval is copied.
   *
   * Returns true if a request was active, false if there was none or the sink has
   * received no snapshot yet. The outcome is in MCAPRingDump::ok. In every case it
   * returns after the writer has finished every dump handed to it so far.
   */
  bool flushPendingDump();

  /// Wait until the writer thread has no dump in progress. Throws std::logic_error if
  /// called from the dump callback.
  void waitForWriter();

  /// Called on the writer thread after each dump, successful or not, until the sink is
  /// destroyed (so also during SinkWorker::stop()): what it captures must outlive the
  /// sink. The callback can call stats(), dumpRequested() and requestDump(), but not
  /// waitForWriter() or flushPendingDump() (std::logic_error). Exceptions it throws are
  /// ignored.
  void setDumpCallback(std::function<void(const MCAPRingDump&)> callback);

  /// All counters, read one by one: while the worker delivers, the fields can come from
  /// slightly different moments. Inline on purpose (built from the getters below), so
  /// MCAPRingStats can gain fields without an ABI break.
  [[nodiscard]] DATA_TAMER_INLINE_LOCAL MCAPRingStats stats() const
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

  /// One getter per MCAPRingStats field (see there). Callable from any thread,
  /// including the dump callback. Not real-time safe: they take a mutex.
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
  // Shared with the writer thread: it outlives a sink destroyed from its dump callback.
  std::shared_ptr<Pimpl> _p;
};

}  // namespace DataTamer
