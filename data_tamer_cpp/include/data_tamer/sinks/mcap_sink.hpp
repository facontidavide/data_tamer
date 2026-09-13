#pragma once

#include "data_tamer/data_sink.hpp"

#include <chrono>
#include <memory>
#include <string>

namespace DataTamer
{

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
   * @param filepath   path of the file to be saved. Should have extension ".mcap"
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

  /// After a certain amount of time, the MCAP file will be reset
  /// and overwritten. Default value is 600 seconds (10 minutes)
  /// To disable this feature, use a time of 0 seconds.
  /// WARNING: this can consume a large amount of disk space very quickly.
  void setMaxTimeBeforeReset(std::chrono::seconds reset_time);

  /// When resetting the MCAP recording (see `setMaxTimeBeforeReset`),
  /// if `create_new_file` is true then the filename will be incremented
  /// and then saved instead of overwriting the previous file.
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

  void openFile(std::string const& filepath);
  void restartRecordingImpl(std::string const& filepath, bool do_compression,
                            bool new_file);
};

}  // namespace DataTamer
