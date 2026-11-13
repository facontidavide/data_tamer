#include "fd_writable.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <utility>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace DataTamer2
{
namespace details
{
namespace
{
// strerror_r() is the XSI one (returns int) or the GNU one (returns a string that may
// be a static one instead of the buffer); overloads pick whichever this libc has.
[[maybe_unused]] const char* strerrorResult(int rc, const char* buffer)
{
  return rc == 0 ? buffer : nullptr;
}
[[maybe_unused]] const char* strerrorResult(const char* text, const char*)
{
  return text;
}
}  // namespace

std::string errnoText(int code)
{
  std::array<char, 256> buffer{};
#if defined(_WIN32)
  const char* text =
      (::strerror_s(buffer.data(), buffer.size(), code) == 0) ? buffer.data() : nullptr;
#else
  const char* text =
      strerrorResult(::strerror_r(code, buffer.data(), buffer.size()), buffer.data());
#endif
  return text != nullptr ? std::string(text) : "errno " + std::to_string(code);
}

FileOps defaultFileOps()
{
  FileOps ops;
#if defined(_WIN32)
  ops.open = [](const char* path) {
    return ::_open(path, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY | _O_NOINHERIT,
                   _S_IREAD | _S_IWRITE);
  };
  ops.write = [](int fd, const void* data, size_t size) -> int64_t {
    // _write() takes an unsigned int: the caller loops over the rest.
    constexpr size_t kMax = size_t(1) << 30;
    return ::_write(fd, data, static_cast<unsigned>(size < kMax ? size : kMax));
  };
  ops.fsync = [](int fd) { return ::_commit(fd); };
  ops.close = [](int fd) { return ::_close(fd); };
#else
  ops.open = [](const char* path) {
    int flags = O_WRONLY | O_CREAT | O_TRUNC;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;  // not inherited by processes the application starts
#endif
    return ::open(path, flags, 0666);
  };
  ops.write = [](int fd, const void* data, size_t size) -> int64_t {
    return ::write(fd, data, size);
  };
  ops.fsync = [](int fd) { return ::fsync(fd); };
  ops.close = [](int fd) { return ::close(fd); };
#endif
#if defined(__linux__)
  static_assert(FdWritable::kSyncWaitBefore == SYNC_FILE_RANGE_WAIT_BEFORE &&
                FdWritable::kSyncWrite == SYNC_FILE_RANGE_WRITE &&
                FdWritable::kSyncWaitAfter == SYNC_FILE_RANGE_WAIT_AFTER);
  ops.sync_range = [](int fd, int64_t offset, int64_t nbytes, unsigned flags) {
    return ::sync_file_range(fd, static_cast<off_t>(offset), static_cast<off_t>(nbytes),
                             flags);
  };
  ops.drop_cache = [](int fd, int64_t offset, int64_t len) {
    return ::posix_fadvise(fd, static_cast<off_t>(offset), static_cast<off_t>(len),
                           POSIX_FADV_DONTNEED);
  };
#endif
  return ops;
}

std::unique_ptr<FdWritable> FdWritable::open(const std::string& path, FileOps ops)
{
  errno = 0;
  const int fd = ops.open(path.c_str());
  if(fd < 0)
  {
    const int code = errno;
    throw std::runtime_error("cannot open " + path + ": " + errnoText(code));
  }
  return std::unique_ptr<FdWritable>(new FdWritable(fd, path, std::move(ops)));
}

FdWritable::FdWritable(int fd, std::string path, FileOps ops)
  : fd_(fd), path_(std::move(path)), ops_(std::move(ops))
{}

FdWritable::~FdWritable()
{
  if(fd_ >= 0)
  {
    ops_.close(fd_);
  }
}

void FdWritable::latch(int code, const char* call)
{
  if(error_ == 0)
  {
    error_ = (code != 0) ? code : EIO;
    error_call_ = call;
  }
}

void FdWritable::handleWrite(const std::byte* data, uint64_t size)
{
  // After an error the file is unusable: writing more could hide the first errno
  // behind a later one.
  if(error_ != 0 || fd_ < 0)
  {
    return;
  }
  const auto* cursor = reinterpret_cast<const char*>(data);
  uint64_t remaining = size;
  while(remaining > 0)
  {
    errno = 0;
    const int64_t written = ops_.write(fd_, cursor, static_cast<size_t>(remaining));
    if(written < 0)
    {
      if(errno == EINTR)
      {
        continue;  // a signal, not a failure: retry the same bytes
      }
      latch(errno, "write");
      return;
    }
    if(written == 0)
    {
      latch(EIO, "write");
      return;
    }
    cursor += written;
    remaining -= static_cast<uint64_t>(written);
    size_ += static_cast<uint64_t>(written);
  }
  flushWindow();
}

void FdWritable::flushWindow()
{
  // Ranges end on a page boundary: the partial last page would be written twice, and
  // POSIX_FADV_DONTNEED keeps partial pages in the cache.
  const uint64_t end = size_ & ~(kPageBytes - 1);
  if(!range_flush_ || !ops_.sync_range || end - writeback_started_ < kFlushWindowBytes)
  {
    return;
  }
  // Start the new window first, so that the disk works on it while this thread waits
  // for the previous one.
  if(!syncRange(writeback_started_, end, kSyncWrite))
  {
    return;
  }
  const uint64_t previous_end = writeback_started_;
  writeback_started_ = end;
  if(previous_end == dropped_)
  {
    return;  // the first window: nothing to wait for yet
  }
  if(!syncRange(dropped_, previous_end, kSyncWaitBefore | kSyncWrite | kSyncWaitAfter))
  {
    return;
  }
  dropCache(dropped_, previous_end);
  dropped_ = previous_end;
}

bool FdWritable::syncRange(uint64_t from, uint64_t to, unsigned flags)
{
  errno = 0;
  if(ops_.sync_range(fd_, static_cast<int64_t>(from), static_cast<int64_t>(to - from),
                     flags) == 0)
  {
    return true;
  }
  const int code = errno;
  if(code == EINVAL || code == ESPIPE || code == ENOSYS || code == EOPNOTSUPP ||
     code == ENOMEM)
  {
    // A file this call cannot handle, or no memory to do it now. Neither consumes the
    // file's writeback error, so the closing fsync() still reports a failed write;
    // only the cache bound is lost.
    range_flush_ = false;
  }
  else
  {
    // With a wait flag the call reports the file's writeback error, with whatever
    // errno the filesystem stored (ENODATA on xfs, ESTALE on NFS), and marks it as
    // seen: the closing fsync() would return 0.
    latch(code, "sync_file_range");
  }
  return false;
}

void FdWritable::dropCache(uint64_t from, uint64_t to) const
{
  if(ops_.drop_cache)
  {
    const int64_t len = (to == 0) ? 0 : static_cast<int64_t>(to - from);
    ops_.drop_cache(fd_, static_cast<int64_t>(from), len);
  }
}

void FdWritable::end()
{
  if(fd_ < 0)
  {
    return;
  }
  errno = 0;
  if(ops_.fsync(fd_) != 0)
  {
    latch(errno, "fsync");
  }
  else
  {
    dropCache(0, 0);  // all of it is on disk: the whole file's cache is clean
  }
  errno = 0;
  if(ops_.close(fd_) != 0)
  {
    latch(errno, "close");
  }
  fd_ = -1;
}

std::string FdWritable::errorMessage() const
{
  if(error_ == 0)
  {
    return {};
  }
  return std::string(error_call_) + " " + path_ + ": " + errnoText(error_);
}

}  // namespace details
}  // namespace DataTamer2
