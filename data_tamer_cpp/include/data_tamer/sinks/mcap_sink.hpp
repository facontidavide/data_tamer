#pragma once

#include "data_tamer/data_sink.hpp"

#include <chrono>
#include <memory>
#include <string>

namespace DataTamer
{

namespace details
{
/// Name of rollover file `number` of `path`: "_<number>" goes before the extension, which
/// is the trailing run of purely alphabetic dot-segments. Directories and the leading
/// dots of a hidden file are not part of it:
///   "dir.d/run.tamer.mcap" -> "dir.d/run_1.tamer.mcap"
///   "data.v2.mcap"         -> "data.v2_1.mcap"
///   "log"                  -> "log_1"
///   ".rec.mcap"            -> ".rec_1.mcap"
std::string NumberedPath(const std::string& path, size_t number);
}  // namespace details

/**
 * @brief Saves the snapshots as an MCAP file (https://mcap.dev/). Create it with
 * MCAPSink::create(), attach the returned worker with LogChannel::addDataSink() and
 * reach the methods below through worker->as<MCAPSink>(). They can be called from any
 * thread.
 *
 * SinkWorker::stop() (and the worker's destructor) delivers what is queued and closes
 * the file. A SinkWorker::start() after a stop() records into the next numbered file,
 * even with setCreateNewFileOnReset(false), unless restartRecording() opened one
 * meanwhile.
 */
class MCAPSink : public DataSink
{
public:
  /**
   * @brief Open `filepath` for writing. An existing file there is overwritten. Throws
   * std::runtime_error if it cannot be opened.
   *
   * @param filepath        should have the extension ".mcap"
   * @param do_compression  zstd-compress the chunks. A crash loses more data than with an
   *                        uncompressed file, so keep it false if the process can crash.
   */
  explicit MCAPSink(std::string const& filepath, bool do_compression = false);

  /// Ready-to-attach sink: SinkWorker::create<MCAPSink>(filepath, do_compression).
  static std::shared_ptr<SinkWorker> create(std::string const& filepath,
                                            bool do_compression = false)
  {
    return SinkWorker::create<MCAPSink>(filepath, do_compression);
  }

  ~MCAPSink() override;

  /// Reset the file at the next snapshot once it is older than `reset_time` (wall clock,
  /// since it was opened). See setCreateNewFileOnReset() for what a reset does.
  /// The default is 600 seconds, and 0 disables resets. Disk usage is unbounded unless
  /// the files are truncated on reset. A rollover that fails is counted in
  /// SinkWorker::errors() and tried again after another `reset_time`, while the current
  /// file keeps recording.
  void setMaxTimeBeforeReset(std::chrono::seconds reset_time);

  /// What a reset does. true (default): continue in a new numbered file, e.g.
  /// "run.tamer.mcap" -> "run_1.tamer.mcap", "run_2.tamer.mcap" (see
  /// details::NumberedPath). Names that exist already, e.g. from a previous run, are
  /// skipped, never overwritten. Nothing is lost, but disk usage is unbounded.
  /// false: truncate and restart the same file. Everything recorded before the reset
  /// is DISCARDED, so only the last `reset_time` of data survives. The recording stops
  /// if the file cannot be opened again.
  void setCreateNewFileOnReset(bool create_new_file);

  /// Close the file and drop later snapshots, until restartRecording() or a
  /// SinkWorker::start() after a stop(). SinkWorker::stop() calls it already: call it
  /// directly only to close the file without stopping the worker.
  void stopRecording();

  /**
   * @brief Close the current file, if still open, and record into `filepath`
   * (overwritten if it exists; `do_compression` as in the constructor). The known
   * schemas are written into the new file, and the rollover numbering restarts from
   * `filepath`. If the file cannot be opened it throws std::runtime_error and keeps the
   * current recording, except when `filepath` is the file in use: that one is closed
   * first, so a failed open stops the sink.
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
