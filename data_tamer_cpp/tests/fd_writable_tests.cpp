// FdWritable, the file output of MCAPSink and MCAPRingSink: error latching and the
// bounded page cache, through injected system calls, plus real files.
#include "sinks/fd_writable.hpp"
#include "mcap_test_utils.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>

using DataTamer2::details::defaultFileOps;
using DataTamer2::details::FdWritable;
using DataTamer2::details::FileOps;
using DataTamerTest::ScratchDir;

namespace
{
constexpr uint64_t kWindow = FdWritable::kFlushWindowBytes;
constexpr unsigned kKick = FdWritable::kSyncWrite;
constexpr unsigned kWait =
    FdWritable::kSyncWaitBefore | FdWritable::kSyncWrite | FdWritable::kSyncWaitAfter;

/// A recorded sync_range or drop_cache call; `flags` is 0 for a drop.
struct RangeCall
{
  enum class Kind
  {
    Sync,
    Drop
  };
  Kind kind;
  int64_t offset;
  int64_t length;
  unsigned flags;

  bool operator==(const RangeCall&) const = default;
};

RangeCall sync(uint64_t offset, uint64_t length, unsigned flags)
{
  return { RangeCall::Kind::Sync, int64_t(offset), int64_t(length), flags };
}
RangeCall drop(uint64_t offset, uint64_t length)
{
  return { RangeCall::Kind::Drop, int64_t(offset), int64_t(length), 0 };
}

/// FileOps whose write() follows a script: each entry is a number of bytes to accept
/// or, when negative, -errno to fail with. After the script, everything is accepted.
struct ScriptedOps
{
  std::vector<int64_t> script;
  size_t calls = 0;
  std::string written;
  int fsync_errno = 0;
  int close_errno = 0;
  int fsync_calls = 0;
  int close_calls = 0;
  int sync_range_errno = 0;
  std::vector<RangeCall> range_calls;

  FileOps ops()
  {
    FileOps ops;
    ops.open = [](const char*) { return 3; };
    ops.write = [this](int, const void* data, size_t size) -> int64_t {
      const int64_t action = (calls < script.size()) ? script[calls] : int64_t(size);
      ++calls;
      if(action < 0)
      {
        errno = int(-action);
        return -1;
      }
      const size_t accepted = std::min(size_t(action), size);
      written.append(static_cast<const char*>(data), accepted);
      return int64_t(accepted);
    };
    ops.fsync = [this](int) {
      ++fsync_calls;
      errno = fsync_errno;
      return fsync_errno == 0 ? 0 : -1;
    };
    ops.close = [this](int) {
      ++close_calls;
      errno = close_errno;
      return close_errno == 0 ? 0 : -1;
    };
    ops.sync_range = [this](int, int64_t offset, int64_t nbytes, unsigned flags) {
      range_calls.push_back({ RangeCall::Kind::Sync, offset, nbytes, flags });
      errno = sync_range_errno;
      return sync_range_errno == 0 ? 0 : -1;
    };
    ops.drop_cache = [this](int, int64_t offset, int64_t len) {
      range_calls.push_back({ RangeCall::Kind::Drop, offset, len, 0 });
      return 0;
    };
    return ops;
  }
};

/// Every write() reaches the injected calls at once, as the tests of them expect.
std::unique_ptr<FdWritable> openUnbuffered(FileOps ops)
{
  return FdWritable::open("/ignored", std::move(ops), 0);
}

void writeString(FdWritable& file, const std::string& text)
{
  file.write(reinterpret_cast<const std::byte*>(text.data()), text.size());
}

/// Writes `total` bytes in 1 MiB pieces.
void writeMiBPieces(FdWritable& file, uint64_t total)
{
  const std::string piece(size_t(1) << 20, 'x');
  for(uint64_t done = 0; done < total; done += piece.size())
  {
    writeString(file, piece);
  }
}

std::string readFile(const std::string& path)
{
  std::ifstream input(path, std::ios::binary);
  return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
}
}  // namespace

TEST(FdWritable, WritesARealFile)
{
  ScratchDir dir("fd_writable_real");
  const auto path = dir.file("out.bin");
  auto file = FdWritable::open(path);
  writeString(*file, "hello mcap");
  EXPECT_EQ(file->size(), 10u);
  file->end();
  EXPECT_EQ(file->error(), 0);
  EXPECT_TRUE(file->errorMessage().empty());
  EXPECT_EQ(readFile(path), "hello mcap");
}

TEST(FdWritable, OpenTruncatesAnExistingFile)
{
  ScratchDir dir("fd_writable_truncate");
  const auto path = dir.file("out.bin");
  std::ofstream(path) << "a longer previous content";
  auto file = FdWritable::open(path);
  writeString(*file, "new");
  file->end();
  EXPECT_EQ(readFile(path), "new");
}

TEST(FdWritable, OpenFailureThrowsWithThePathAndTheReason)
{
  ScratchDir dir("fd_writable_open");
  const auto path = dir.file("missing/out.bin");
  try
  {
    FdWritable::open(path);
    FAIL() << "open() did not throw";
  }
  catch(const std::runtime_error& e)
  {
    const std::string message = e.what();
    EXPECT_NE(message.find(path), std::string::npos) << message;
    EXPECT_NE(message.find(DataTamer2::details::errnoText(ENOENT)), std::string::npos)
        << message;
  }
}

TEST(FdWritable, PartialWritesAreLoopedToCompletion)
{
  ScriptedOps scripted;
  scripted.script = { 4, 3 };
  auto file = openUnbuffered(scripted.ops());
  writeString(*file, "0123456789");
  EXPECT_EQ(scripted.written, "0123456789");
  EXPECT_EQ(file->size(), 10u);
  EXPECT_EQ(file->error(), 0);
  EXPECT_EQ(scripted.calls, 3u);
}

TEST(FdWritable, EintrIsRetriedNotLatched)
{
  ScriptedOps scripted;
  scripted.script = { -EINTR, 2, -EINTR };
  auto file = openUnbuffered(scripted.ops());
  writeString(*file, "abcdef");
  EXPECT_EQ(scripted.written, "abcdef");
  EXPECT_EQ(file->error(), 0);
}

TEST(FdWritable, TheFirstErrnoIsLatchedAndWritingStops)
{
  ScriptedOps scripted;
  scripted.script = { 5, -ENOSPC };
  auto file = openUnbuffered(scripted.ops());
  writeString(*file, "0123456789");
  EXPECT_EQ(file->error(), ENOSPC);
  EXPECT_EQ(file->size(), 5u);

  const size_t calls = scripted.calls;
  writeString(*file, "0123456789");
  EXPECT_EQ(scripted.calls, calls);  // nothing more reaches the descriptor
  file->end();
  EXPECT_EQ(file->error(), ENOSPC);  // not replaced by a later error
  EXPECT_NE(file->errorMessage().find("write /ignored"), std::string::npos)
      << file->errorMessage();
}

TEST(FdWritable, AWriteOfZeroBytesIsAnError)
{
  ScriptedOps scripted;
  scripted.script = { 0 };
  auto file = openUnbuffered(scripted.ops());
  writeString(*file, "abc");
  EXPECT_EQ(file->error(), EIO);
}

TEST(FdWritable, EndLatchesAFailedFsyncAndStillCloses)
{
  ScriptedOps scripted;
  scripted.fsync_errno = EIO;
  auto file = openUnbuffered(scripted.ops());
  writeMiBPieces(*file, 1u << 20);
  file->end();
  EXPECT_EQ(file->error(), EIO);
  EXPECT_NE(file->errorMessage().find("fsync"), std::string::npos);
  EXPECT_EQ(scripted.close_calls, 1);
  EXPECT_TRUE(scripted.range_calls.empty());  // a failed fsync keeps the cache

  file->end();  // idempotent
  EXPECT_EQ(scripted.fsync_calls, 1);
  EXPECT_EQ(scripted.close_calls, 1);
}

TEST(FdWritable, EndLatchesAFailedClose)
{
  ScriptedOps scripted;
  scripted.close_errno = EIO;
  auto file = openUnbuffered(scripted.ops());
  file->end();
  EXPECT_EQ(file->error(), EIO);
  EXPECT_NE(file->errorMessage().find("close"), std::string::npos);
}

TEST(FdWritable, TheDestructorClosesAFileNotEnded)
{
  ScriptedOps scripted;
  openUnbuffered(scripted.ops()).reset();
  EXPECT_EQ(scripted.close_calls, 1);
  EXPECT_EQ(scripted.fsync_calls, 0);
}

TEST(FdWritable, FlushesEachWindowAndDropsThePreviousOne)
{
  ScriptedOps scripted;
  auto file = openUnbuffered(scripted.ops());
  writeMiBPieces(*file, 3 * kWindow);
  const std::vector<RangeCall> expected{
    sync(0, kWindow, kKick),            //
    sync(kWindow, kWindow, kKick),      // start the second window, then
    sync(0, kWindow, kWait),            // wait for the first one
    drop(0, kWindow),                   // and drop it
    sync(2 * kWindow, kWindow, kKick),  //
    sync(kWindow, kWindow, kWait),      //
    drop(kWindow, kWindow),
  };
  EXPECT_EQ(scripted.range_calls, expected);

  file->end();  // the fsync, then one drop of the whole file
  EXPECT_EQ(file->error(), 0);
  ASSERT_EQ(scripted.range_calls.size(), expected.size() + 1);
  EXPECT_EQ(scripted.range_calls.back(), drop(0, 0));
}

TEST(FdWritable, AFileSmallerThanAWindowIsOnlyDroppedAtTheEnd)
{
  ScriptedOps scripted;
  auto file = openUnbuffered(scripted.ops());
  writeMiBPieces(*file, kWindow - (1u << 20));
  EXPECT_TRUE(scripted.range_calls.empty());
  file->end();
  EXPECT_EQ(scripted.range_calls, std::vector<RangeCall>{ drop(0, 0) });
}

TEST(FdWritable, WithoutRangeCallsOnlyTheFsyncRuns)
{
  ScriptedOps scripted;
  FileOps ops = scripted.ops();
  ops.sync_range = nullptr;  // a platform without sync_file_range()
  ops.drop_cache = nullptr;
  auto file = openUnbuffered(ops);
  writeMiBPieces(*file, 3 * kWindow);
  file->end();
  EXPECT_EQ(file->error(), 0);
  EXPECT_EQ(scripted.fsync_calls, 1);
  EXPECT_EQ(scripted.written.size(), 3 * kWindow);
}

TEST(FdWritable, ASyncRangeDataErrorIsLatched)
{
  ScriptedOps scripted;
  scripted.sync_range_errno = EIO;
  auto file = openUnbuffered(scripted.ops());
  writeMiBPieces(*file, 2 * kWindow);
  EXPECT_EQ(file->error(), EIO);
  EXPECT_NE(file->errorMessage().find("sync_file_range"), std::string::npos);
  EXPECT_EQ(file->size(), kWindow);  // nothing written after the error
  EXPECT_EQ(scripted.range_calls.size(), 1u);
}

// xfs reports a medium error as ENODATA. sync_file_range() with a wait flag consumes
// the error, so the closing fsync would return 0: the file must fail here.
TEST(FdWritable, AnUnexpectedSyncRangeErrorIsLatchedToo)
{
  ScriptedOps scripted;
  scripted.sync_range_errno = ENODATA;
  auto file = openUnbuffered(scripted.ops());
  writeMiBPieces(*file, 2 * kWindow);
  EXPECT_EQ(file->error(), ENODATA);
}

// The file does not support the call (EINVAL, ESPIPE, ENOSYS, EOPNOTSUPP) or the
// kernel has no memory for it now (ENOMEM): range flushing stops and the closing
// fsync decides. Under memory pressure a correct file must not be reported as failed.
TEST(FdWritable, AnUnsupportedOrOutOfMemorySyncRangeLeavesTheVerdictToFsync)
{
  for(const int code : { EINVAL, ESPIPE, ENOSYS, EOPNOTSUPP, ENOMEM })
  {
    SCOPED_TRACE(code);
    ScriptedOps scripted;
    scripted.sync_range_errno = code;
    auto file = openUnbuffered(scripted.ops());
    writeMiBPieces(*file, 3 * kWindow);
    EXPECT_EQ(file->error(), 0);
    EXPECT_EQ(file->size(), 3 * kWindow);
    EXPECT_EQ(scripted.range_calls.size(), 1u);  // tried once, then turned off
    file->end();
    EXPECT_EQ(file->error(), 0);
    EXPECT_EQ(scripted.fsync_calls, 1);
  }
}

TEST(FdWritable, AFailedWaitSkipsTheDrop)
{
  ScriptedOps scripted;
  FileOps ops = scripted.ops();
  ops.sync_range = [&scripted](int, int64_t offset, int64_t nbytes, unsigned flags) {
    scripted.range_calls.push_back({ RangeCall::Kind::Sync, offset, nbytes, flags });
    if((flags & FdWritable::kSyncWaitAfter) != 0)
    {
      errno = EIO;
      return -1;
    }
    return 0;
  };
  auto file = openUnbuffered(ops);
  writeMiBPieces(*file, 2 * kWindow);
  const std::vector<RangeCall> expected{
    sync(0, kWindow, kKick),
    sync(kWindow, kWindow, kKick),
    sync(0, kWindow, kWait),
  };
  EXPECT_EQ(scripted.range_calls, expected);  // no drop after the failed wait
  EXPECT_EQ(file->error(), EIO);
}

TEST(FdWritable, UnalignedWritesFlushWholePagesWithoutGaps)
{
  ScriptedOps scripted;
  auto file = openUnbuffered(scripted.ops());
  const std::string piece(777777, 'x');
  while(file->size() < 2 * kWindow + piece.size())
  {
    writeString(*file, piece);
  }
  ASSERT_FALSE(scripted.range_calls.empty());
  int64_t next = 0;
  for(const RangeCall& call : scripted.range_calls)
  {
    EXPECT_EQ(call.offset % int64_t(FdWritable::kPageBytes), 0);
    EXPECT_EQ(call.length % int64_t(FdWritable::kPageBytes), 0);
    if(call.kind == RangeCall::Kind::Sync && call.flags == kKick)
    {
      EXPECT_EQ(call.offset, next);
      next = call.offset + call.length;
    }
  }
  EXPECT_GE(next, int64_t(2 * kWindow));
}

TEST(FdWritable, SmallWritesAreCollectedIntoOneWrite)
{
  ScriptedOps scripted;
  auto file = FdWritable::open("/ignored", scripted.ops());
  writeString(*file, "abc");
  writeString(*file, "defg");
  EXPECT_EQ(scripted.calls, 0u);
  EXPECT_EQ(file->size(), 7u);  // buffered bytes count, as McapWriter needs
  file->end();
  EXPECT_EQ(scripted.calls, 1u);
  EXPECT_EQ(scripted.written, "abcdefg");
}

TEST(FdWritable, ALargeWriteGoesAfterTheBufferedBytes)
{
  ScriptedOps scripted;
  auto file = FdWritable::open("/ignored", scripted.ops());
  const std::string large(FdWritable::kBufferBytes, 'L');
  writeString(*file, "small");
  writeString(*file, large);
  EXPECT_EQ(scripted.calls, 2u);
  EXPECT_EQ(scripted.written, "small" + large);
  file->end();
  EXPECT_EQ(scripted.calls, 2u);  // nothing left in the buffer
}

TEST(FdWritable, AFailedWriteOfTheBufferIsLatchedByEnd)
{
  ScriptedOps scripted;
  scripted.script = { -ENOSPC };
  auto file = FdWritable::open("/ignored", scripted.ops());
  writeString(*file, "abc");
  EXPECT_EQ(file->error(), 0);
  file->end();
  EXPECT_EQ(file->error(), ENOSPC);
  EXPECT_EQ(scripted.close_calls, 1);
}

// fsync() fails with EINVAL (or EROFS) on a file that cannot be synchronized, a FIFO
// or /dev/null: the data was written, so the file is not reported as failed.
TEST(FdWritable, AFileThatCannotBeSyncedIsNotAFailure)
{
  for(const int code : { EINVAL, EROFS })
  {
    SCOPED_TRACE(code);
    ScriptedOps scripted;
    scripted.fsync_errno = code;
    auto file = openUnbuffered(scripted.ops());
    writeString(*file, "abc");
    file->end();
    EXPECT_EQ(file->error(), 0) << file->errorMessage();
    EXPECT_EQ(scripted.close_calls, 1);
  }
}

TEST(FdWritable, ARealFifoIsWrittenWithoutError)
{
  ScratchDir dir("fd_writable_fifo");
  const auto path = dir.file("pipe");
  ASSERT_EQ(::mkfifo(path.c_str(), 0600), 0);
  std::string received;
  std::thread reader([&] { received = readFile(path); });
  auto file = FdWritable::open(path);
  writeString(*file, "through a pipe");
  file->end();
  reader.join();
  EXPECT_EQ(file->error(), 0) << file->errorMessage();
  EXPECT_EQ(received, "through a pipe");
}

// McapWriter calls IWritable::flush() after every chunk: the chunk then reaches the
// file, so a crash loses only the chunk being filled. Chunks are smaller than the
// buffer here, as compressed chunks often are.
TEST(FdWritable, EveryChunkReachesTheFileWhenMcapFlushesIt)
{
  if constexpr(!requires(mcap::IWritable& w) { w.flush(); })
  {
    GTEST_SKIP() << "this MCAP release has no IWritable::flush()";
  }
  else
  {
    ScratchDir dir("fd_writable_chunks");
    const auto path = dir.file("chunks.mcap");
    auto file = FdWritable::open(path);
    mcap::McapWriter writer;
    mcap::McapWriterOptions options("test");
    options.compression = mcap::Compression::None;
    options.chunkSize = 1024;
    writer.open(*file, options);
    mcap::Schema schema("s", "jsonschema", "{}");
    writer.addSchema(schema);
    mcap::Channel channel("t", "json", schema.id);
    writer.addChannel(channel);
    const std::string payload(100, 'x');
    for(uint32_t i = 0; i < 50; ++i)  // 5000 bytes: several chunks
    {
      mcap::Message message;
      message.channelId = channel.id;
      message.sequence = i;
      message.logTime = i;
      message.publishTime = i;
      message.data = reinterpret_cast<const std::byte*>(payload.data());
      message.dataSize = payload.size();
      ASSERT_TRUE(writer.write(message).ok());
    }
    EXPECT_GT(std::filesystem::file_size(path), 4000u);  // before close
    writer.close();
    EXPECT_EQ(file->error(), 0) << file->errorMessage();
  }
}

// Window boundaries fall inside uneven writes, on the real system calls.
TEST(FdWritable, ARealFileOfSeveralWindowsKeepsItsBytes)
{
  ScratchDir dir("fd_writable_big");
  const auto path = dir.file("big.bin");
  const uint64_t total = 3 * kWindow + 12345;
  std::string pattern(total, '\0');
  for(uint64_t i = 0; i < total; ++i)
  {
    pattern[i] = static_cast<char>(i * 31u);
  }
  auto file = FdWritable::open(path);
  // Large writes that go straight to the file, mixed with small ones that are buffered.
  const uint64_t pieces[] = { 777777, 13, 70000, 1000, 8 };
  uint64_t done = 0;
  for(size_t i = 0; done < total; ++i)
  {
    const uint64_t n = std::min(pieces[i % std::size(pieces)], total - done);
    file->write(reinterpret_cast<const std::byte*>(pattern.data() + done), n);
    done += n;
  }
  file->end();
  ASSERT_EQ(file->error(), 0) << file->errorMessage();
  EXPECT_TRUE(readFile(path) == pattern);
}

#if defined(__linux__)
TEST(FdWritable, DefaultFileOpsHaveTheRangeCallsOnLinux)
{
  const FileOps ops = defaultFileOps();
  EXPECT_TRUE(ops.sync_range);
  EXPECT_TRUE(ops.drop_cache);
}
#endif
