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
 * worker->as<MCAPSink>().
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

  /// After a certain amount of time, the MCAP file is closed and the recording
  /// continues in a new file (see setCreateNewFileOnReset). Default value is
  /// 600 seconds (10 minutes). To disable this feature, use a time of 0 seconds.
  /// WARNING: without a reset the file grows for as long as the application runs.
  void setMaxTimeBeforeReset(std::chrono::seconds reset_time);

  /// What happens on a reset (see `setMaxTimeBeforeReset`).
  /// If `create_new_file` is true (default), the recording continues in a new
  /// file whose name carries a counter, inserted before the extension:
  /// "run.tamer.mcap" -> "run_1.tamer.mcap", "run_2.tamer.mcap", ...
  /// (see details::NumberedPath). Numbered names that already exist, e.g. from
  /// a previous run with the same path, are skipped, never overwritten.
  /// Nothing is lost, but disk usage is unbounded.
  /// If false, the same file is truncated and restarted: everything recorded
  /// before the reset is DISCARDED. Use it only to bound disk usage.
  void setCreateNewFileOnReset(bool create_new_file);

  /// Stop recording and save the file. Snapshots delivered afterwards are
  /// dropped until restartRecording(). To also deliver what is still queued,
  /// call SinkWorker::stop() (or drain()) first.
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

private:
  struct Pimpl;
  std::unique_ptr<Pimpl> _p;

  void openFile(std::string const& filepath, bool do_compression);
  void restartRecordingImpl(std::string const& filepath, bool do_compression,
                            bool new_file);
};

}  // namespace DataTamer
