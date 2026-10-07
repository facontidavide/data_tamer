#pragma once

#include "data_tamer/data_sink.hpp"

#include <chrono>
#include <memory>
#include <string>

namespace DataTamer
{

namespace details
{
/// File name used for rollover number `number` of `path`: "_<number>" is
/// inserted before the extension of the file name (directories are left alone).
/// The extension is the trailing run of purely alphabetic dot-segments, so
/// multi-part extensions survive and dotted stems stay whole:
///   "dir.d/run.tamer.mcap" -> "dir.d/run_1.tamer.mcap"
///   "log_2026.10.05.mcap"  -> "log_2026.10.05_1.mcap"
///   "data.v2.mcap"         -> "data.v2_1.mcap"
/// A name without such an extension gets the counter appended ("log" -> "log_1",
/// "log.123" -> "log.123_1"); the leading dots of a hidden file belong to the
/// name (".rec.mcap" -> ".rec_1.mcap", ".rec" -> ".rec_1").
std::string NumberedPath(const std::string& path, size_t number);
}  // namespace details

/**
 * @brief The MCAPSink is a DataSink that saves the data as an MCAP file
 * (https://mcap.dev/). Create it with MCAPSink::create() and pass the returned
 * worker to LogChannel::addDataSink(); reach the methods below through
 * worker->as<MCAPSink>(). SinkWorker::stop() (and the worker's destructor)
 * delivers what is queued and closes the file. SinkWorker::start() after a
 * stop() records into the next numbered file (details::NumberedPath), even when
 * resets truncate the same file (setCreateNewFileOnReset(false)), unless
 * restartRecording() opened one meanwhile.
 */
class MCAPSink : public DataSink
{
public:
  /**
   * @brief MCAPSink.
   * IMPORTANT: if you want the recorder to be more robust to crash/segfault,
   * set `do_compression` to false.
   * Compression is safe if your application is closing cleanly.
   *
   * @param filepath   path of the file to be saved. Should have extension ".mcap".
   *                   An existing file at this path is overwritten; rollover files
   *                   (see setCreateNewFileOnReset) never overwrite existing ones.
   * @param do_compression if true, compress the data on the fly.
   */
  explicit MCAPSink(std::string const& filepath, bool do_compression = false);

  /// Ready-to-attach sink: SinkWorker::create<MCAPSink>(filepath, do_compression).
  static std::shared_ptr<SinkWorker> create(std::string const& filepath,
                                            bool do_compression = false)
  {
    return SinkWorker::create<MCAPSink>(filepath, do_compression);
  }

  ~MCAPSink() override;

  /// After a certain amount of time, the MCAP file is reset: by default the
  /// recording continues in a new numbered file and nothing is lost (see
  /// setCreateNewFileOnReset to truncate and overwrite the same file instead).
  /// Default value is 600 seconds (10 minutes). To disable this feature, use a
  /// time of 0 seconds.
  /// WARNING: disk usage is unbounded unless the files are truncated on reset
  /// (setCreateNewFileOnReset(false)): a new file every `reset_time`, for as
  /// long as the application runs.
  void setMaxTimeBeforeReset(std::chrono::seconds reset_time);

  /// What happens on a reset (see `setMaxTimeBeforeReset`).
  /// If `create_new_file` is true (default), the recording continues in a new
  /// file whose name carries a counter, inserted before the extension:
  /// "run.tamer.mcap" -> "run_1.tamer.mcap", "run_2.tamer.mcap", ...
  /// (see details::NumberedPath). Numbered names that already exist, e.g. from
  /// a previous run with the same path, are skipped, never overwritten.
  /// Nothing is lost, but disk usage is unbounded.
  /// If false, the same file is truncated and restarted: everything recorded
  /// before the reset is DISCARDED. This bounds disk usage, but keeps at most
  /// the last `reset_time` of data.
  void setCreateNewFileOnReset(bool create_new_file);

  /// Stop recording and save the file. Snapshots delivered afterwards are
  /// dropped until restartRecording() or SinkWorker::start() after a stop().
  /// SinkWorker::stop() calls it after delivering what is still queued, so
  /// stopping the worker is enough to get a complete file; call it directly to
  /// close the file without stopping.
  void stopRecording();

  /**
   * @brief restartRecording saves the current file (unless we did it already,
   * calling stopRecording) and start recording into a new one.
   * Note that all the registered channels and their schemas will be copied into the new file.
   *
   * @param filepath   file path of the new file (should be ".mcap" extension)
   * @param do_compression if true, compress the data on the fly.
   * WARNING: if this is called with the same filename as previously, the file counter will be reset, too.
   */
  void restartRecording(std::string const& filepath, bool do_compression = false);

protected:
  void onSchema(Schema const& schema) override;
  void onSnapshot(const SnapshotRef& snapshot) override;
  /// Closes the file, as stopRecording().
  void onStop() override;
  /// If the file is closed, records into the next numbered file.
  void onStart() override;

private:
  struct Pimpl;
  std::unique_ptr<Pimpl> _p;

  void openFile(std::string const& filepath, bool do_compression);
  void openNextNumberedFile();
  void restartRecordingImpl(std::string const& filepath, bool do_compression,
                            bool new_file);
};

}  // namespace DataTamer
