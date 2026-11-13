#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <mcap/writer.hpp>

namespace DataTamer2
{
namespace details
{
/// The system calls FdWritable makes, injectable so that a test can fail any of them
/// with a chosen errno. Each callable returns what the real call returns and sets
/// `errno` the way it does, except `fadvise`, which returns the error number like
/// posix_fadvise().
struct FileOps
{
  /// open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666): a descriptor, or -1.
  std::function<int(const char* path)> open;
  /// write(): bytes written, or -1.
  std::function<int64_t(int fd, const void* data, size_t size)> write;
  std::function<int(int fd)> fsync;
  std::function<int(int fd)> close;
  /// sync_file_range(fd, offset, nbytes, flags), with the kSync* flags of FdWritable.
  /// Empty where the platform has no such call: no writeback during the write, only
  /// the fsync() at the end.
  std::function<int(int fd, int64_t offset, int64_t nbytes, unsigned flags)> sync_range;
  /// posix_fadvise(fd, offset, len, POSIX_FADV_DONTNEED). Empty where the platform has
  /// no such call: nothing is dropped from the page cache.
  std::function<int(int fd, int64_t offset, int64_t len)> drop_cache;
};

/// FileOps bound to the real calls of this platform.
FileOps defaultFileOps();

/// strerror() through the reentrant strerror_r(), for messages built on sink threads.
std::string errnoText(int code);

/**
 * The mcap::IWritable of MCAPSink and MCAPRingSink. Two reasons not to use the
 * mcap::FileWriter or a std::ofstream:
 *
 * - Errors. FileWriter checks fwrite() with an assert that release builds compile out,
 *   so a full disk would produce a truncated file and no error. Here the first errno
 *   is latched, nothing more is written after it, and end() runs fsync() then close()
 *   and latches their errors too.
 *
 * - Page cache (Linux). Without it, a file of N bytes leaves up to N bytes of dirty
 *   page cache for the kernel to flush later, all at once at the end of a dump. On a
 *   robot computer that floods the disk queue and makes other processes wait for
 *   their page faults. Every kFlushWindowBytes this class starts the writeback of the
 *   bytes written since the last window, waits for the previous window to reach the
 *   disk and drops it from the cache. At most about three windows of the file are in
 *   memory, and the writer runs no further ahead of the disk than that. end() drops
 *   the rest after the fsync(). The bound does not hold on overlayfs or tmpfs.
 */
class FdWritable final : public mcap::IWritable
{
public:
  /// Bytes between two range flushes: long sequential writes for the disk, and three
  /// of them are small next to the sinks' own buffers.
  static constexpr uint64_t kFlushWindowBytes = uint64_t(8) << 20;

  /// Flushed ranges end on a multiple of this, the page size on x86_64 and arm64 with
  /// 4 KiB pages. A larger page only leaves one partial page per window in the cache.
  static constexpr uint64_t kPageBytes = 4096;

  /// The sync_file_range() flags, with the Linux values.
  static constexpr unsigned kSyncWaitBefore = 1;
  static constexpr unsigned kSyncWrite = 2;
  static constexpr unsigned kSyncWaitAfter = 4;

  /// Creates or truncates `path`. Throws std::runtime_error, with the path and the
  /// reason, if it cannot be opened.
  static std::unique_ptr<FdWritable> open(const std::string& path,
                                          FileOps ops = defaultFileOps());

  FdWritable(const FdWritable&) = delete;
  FdWritable& operator=(const FdWritable&) = delete;
  ~FdWritable() override;

  /// fsync() then close(), latching the first error of either. Idempotent.
  void end() override;

  /// Bytes handed to the descriptor so far.
  uint64_t size() const override { return size_; }

  /// The latched errno, 0 if no call failed.
  int error() const { return error_; }

  /// "<call> <path>: <reason>" for the latched error, empty if there is none.
  std::string errorMessage() const;

protected:
  void handleWrite(const std::byte* data, uint64_t size) override;

private:
  FdWritable(int fd, std::string path, FileOps ops);

  void latch(int code, const char* call);

  /// The range flush of the class comment, once kFlushWindowBytes have been written
  /// since the last one.
  void flushWindow();

  /// ops_.sync_range on [from, to). False if it failed. EINVAL, ESPIPE, ENOSYS and
  /// EOPNOTSUPP mean the file does not support it, and ENOMEM that the kernel could
  /// not do it now: range flushing is turned off, and the closing fsync() decides
  /// whether the data is on disk. Any other error is latched like a write() error.
  bool syncRange(uint64_t from, uint64_t to, unsigned flags);

  /// Drops [from, to) from the page cache, `to == 0` meaning the end of the file.
  /// Advisory: a failure is ignored.
  void dropCache(uint64_t from, uint64_t to) const;

  int fd_ = -1;
  std::string path_;
  FileOps ops_;
  uint64_t size_ = 0;
  int error_ = 0;
  const char* error_call_ = "";
  bool range_flush_ = true;
  uint64_t writeback_started_ = 0;  // end of the last range whose writeback started
  uint64_t dropped_ = 0;            // end of the last range on disk and out of the cache
};

}  // namespace details
}  // namespace DataTamer2
