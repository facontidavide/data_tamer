#include "data_tamer/sinks/mcap_ring_sink.hpp"

// The MCAP implementation is compiled in mcap_sink.cpp.
#include "data_tamer/sinks/mcap_encoding.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

namespace DataTamer
{
namespace
{
using std::chrono::nanoseconds;

// requestDump() word: the top bit means "pending", the other bits hold the
// post-trigger delay in nanoseconds. Zero means no active request.
constexpr uint64_t kPending = uint64_t{ 1 } << 63;
constexpr uint64_t kMaxDelay = kPending - 1;
constexpr nanoseconds kMinTime(std::numeric_limits<nanoseconds::rep>::min());

nanoseconds saturatingAdd(nanoseconds a, nanoseconds b)
{
  const auto max = std::numeric_limits<nanoseconds::rep>::max();
  return (b.count() > 0 && a.count() > max - b.count()) ? nanoseconds(max) : a + b;
}

nanoseconds saturatingSub(nanoseconds a, nanoseconds b)
{
  const auto min = std::numeric_limits<nanoseconds::rep>::min();
  return (b.count() > 0 && a.count() < min + b.count()) ? nanoseconds(min) : a - b;
}

// Each stored snapshot is this header followed by mask and payload bytes.
// Records are packed back to back and copied with memcpy, so no alignment.
struct RecordHeader
{
  uint64_t schema_hash;
  int64_t timestamp;
  uint32_t mask_size;
  uint32_t payload_size;

  size_t recordSize() const { return sizeof(RecordHeader) + mask_size + payload_size; }
};

// Byte ring of records. A record may wrap around the end of the buffer.
class ByteRing
{
public:
  explicit ByteRing(size_t capacity) : buffer_(capacity) {}

  size_t capacity() const { return buffer_.size(); }
  size_t used() const { return used_; }
  size_t count() const { return count_; }

  RecordHeader front() const
  {
    RecordHeader header;
    read(head_, &header, sizeof(header));
    return header;
  }

  void popFront()
  {
    const size_t size = front().recordSize();
    head_ = (head_ + size) % capacity();
    used_ -= size;
    --count_;
  }

  // The caller has made room: used() + header.recordSize() <= capacity().
  void push(const RecordHeader& header, const uint8_t* mask, const uint8_t* payload)
  {
    size_t pos = (head_ + used_) % capacity();
    pos = write(pos, &header, sizeof(header));
    pos = write(pos, mask, header.mask_size);
    write(pos, payload, header.payload_size);
    used_ += header.recordSize();
    ++count_;
  }

  // Copy the records whose timestamp is in [first, last], oldest first, into
  // `out` (at least used() bytes). Returns the number of bytes copied.
  size_t copyRange(nanoseconds first, nanoseconds last, uint8_t* out) const
  {
    size_t copied = 0;
    size_t pos = head_;
    for(size_t i = 0; i < count_; ++i)
    {
      RecordHeader header;
      read(pos, &header, sizeof(header));
      const size_t size = header.recordSize();
      const nanoseconds timestamp(header.timestamp);
      if(timestamp >= first && timestamp <= last)
      {
        read(pos, out + copied, size);
        copied += size;
      }
      pos = (pos + size) % capacity();
    }
    return copied;
  }

private:
  size_t write(size_t pos, const void* src, size_t size)
  {
    if(size == 0)
    {
      return pos;
    }
    const auto* bytes = static_cast<const uint8_t*>(src);
    const size_t first = std::min(size, capacity() - pos);
    std::memcpy(buffer_.data() + pos, bytes, first);
    std::memcpy(buffer_.data(), bytes + first, size - first);
    return (pos + size) % capacity();
  }

  void read(size_t pos, void* dst, size_t size) const
  {
    if(size == 0)
    {
      return;
    }
    auto* bytes = static_cast<uint8_t*>(dst);
    const size_t first = std::min(size, capacity() - pos);
    std::memcpy(bytes, buffer_.data() + pos, first);
    std::memcpy(bytes + first, buffer_.data(), size - first);
  }

  std::vector<uint8_t> buffer_;
  size_t head_ = 0;
  size_t used_ = 0;
  size_t count_ = 0;
};

}  // namespace

struct MCAPRingSink::Pimpl
{
  explicit Pimpl(MCAPRingOptions opt)
    : options(std::move(opt))
    , ring(options.capacity_bytes)
    , dump_buffer(options.capacity_bytes)
  {}

  // Ring side: touched by onSnapshot() and flushPendingDump().
  enum class Phase
  {
    Idle,        // no request triggered yet
    Collecting,  // triggered, waiting for the post-trigger interval
    Ready        // complete, waiting for the writer
  };

  const MCAPRingOptions options;
  std::atomic<uint64_t> request{ 0 };

  mutable std::mutex ring_mutex;
  ByteRing ring;
  Phase phase = Phase::Idle;
  bool retry_counted = false;  // the active dump already found the writer busy
  nanoseconds trigger_time{ 0 }, dump_start{ 0 }, dump_end{ 0 };
  bool has_snapshot = false;
  nanoseconds max_timestamp = kMinTime;
  // Newest timestamp evicted by capacity: a dump starting at or before it lost data.
  nanoseconds capacity_evicted_until = kMinTime;
  uint64_t writer_busy_retries = 0;
  uint64_t evicted_by_capacity = 0;
  uint64_t dropped_oversize = 0;

  // Schemas, inserted by onSchema() and read by the writer thread. Entries are
  // never modified once inserted, so the writer uses them outside the lock.
  std::mutex schema_mutex;
  std::map<uint64_t, Schema> schemas;

  // dump_buffer and dump_size are written by the ring_mutex holder while
  // !writer_busy and read by the writer thread while writer_busy; `job` is
  // owned by the writer thread while writer_busy.
  std::vector<uint8_t> dump_buffer;
  size_t dump_size = 0;

  mutable std::mutex writer_mutex;
  std::condition_variable writer_cv;
  bool writer_busy = false;
  bool writer_stop = false;
  MCAPRingDump job;
  size_t dump_counter = 0;  // writer thread only
  uint64_t dumps_written = 0;
  uint64_t dumps_failed = 0;
  std::function<void(const MCAPRingDump&)> callback;
  std::thread writer_thread;

  void store(const Snapshot& snapshot);
  bool tryHandOff();
  void writerLoop();
  void writeDump(MCAPRingDump& dump);
  void throwIfWriterThread(const char* function) const
  {
    if(std::this_thread::get_id() == writer_thread.get_id())
    {
      throw std::logic_error(std::string("MCAPRingSink::") + function +
                             " called from the dump callback");
    }
  }
};

// Caller holds ring_mutex.
void MCAPRingSink::Pimpl::store(const Snapshot& snapshot)
{
  // Evict by age. While a dump is active, keep everything it may contain.
  nanoseconds horizon = saturatingSub(snapshot.timestamp, options.window);
  if(phase != Phase::Idle)
  {
    horizon = std::min(horizon, dump_start);
  }
  while(ring.count() > 0 && nanoseconds(ring.front().timestamp) < horizon)
  {
    ring.popFront();
  }

  const size_t size =
      sizeof(RecordHeader) + snapshot.active_mask.size() + snapshot.payload.size();
  if(size > ring.capacity() || snapshot.active_mask.size() > UINT32_MAX ||
     snapshot.payload.size() > UINT32_MAX)
  {
    ++dropped_oversize;
    return;
  }
  // Evict by capacity.
  while(ring.used() + size > ring.capacity())
  {
    capacity_evicted_until =
        std::max(capacity_evicted_until, nanoseconds(ring.front().timestamp));
    ring.popFront();
    ++evicted_by_capacity;
  }
  const RecordHeader header{ snapshot.schema_hash, snapshot.timestamp.count(),
                             static_cast<uint32_t>(snapshot.active_mask.size()),
                             static_cast<uint32_t>(snapshot.payload.size()) };
  ring.push(header, snapshot.active_mask.data(), snapshot.payload.data());
}

// Caller holds ring_mutex and phase == Ready. Never waits: returns false if
// the writer is busy. The copy runs under ring_mutex only; the writer does not
// touch dump_buffer until writer_busy is set below.
bool MCAPRingSink::Pimpl::tryHandOff()
{
  {
    std::scoped_lock lock(writer_mutex);
    if(writer_busy)
    {
      if(!retry_counted)
      {
        ++writer_busy_retries;
        retry_counted = true;
      }
      return false;
    }
  }
  dump_size = ring.copyRange(dump_start, dump_end, dump_buffer.data());
  {
    std::scoped_lock lock(writer_mutex);
    // clear() keeps the capacity: the worker thread allocates nothing here.
    job.path.clear();
    job.error.clear();
    job.trigger_time = trigger_time;
    job.start = dump_start;
    job.end = dump_end;
    job.first_message = job.last_message = nanoseconds(0);
    job.messages = 0;
    job.truncated =
        capacity_evicted_until >= dump_start && capacity_evicted_until <= dump_end;
    job.ok = false;
    writer_busy = true;
  }
  writer_cv.notify_all();
  phase = Phase::Idle;
  retry_counted = false;
  request.store(0, std::memory_order_release);
  return true;
}

void MCAPRingSink::Pimpl::writerLoop()
{
  std::unique_lock lock(writer_mutex);
  while(true)
  {
    writer_cv.wait(lock, [this] { return writer_busy || writer_stop; });
    if(!writer_busy)
    {
      return;  // stop requested and nothing left to write
    }
    lock.unlock();
    writeDump(job);
    lock.lock();
    // The callback runs without writer_mutex, so it may call stats() and
    // requestDump(); writer_busy stays true until it returns.
    std::function<void(const MCAPRingDump&)> notify;
    try
    {
      notify = callback;
    }
    catch(...)
    {}
    lock.unlock();
    if(notify)
    {
      try
      {
        notify(job);
      }
      catch(...)
      {
        // A throwing callback must not kill the writer thread.
      }
    }
    lock.lock();
    ++(job.ok ? dumps_written : dumps_failed);
    writer_busy = false;
    writer_cv.notify_all();
  }
}

// Runs on the writer thread, which owns dump_buffer and `dump`.
void MCAPRingSink::Pimpl::writeDump(MCAPRingDump& dump)
{
  try
  {
    // Skip the names that exist already, e.g. dumps of a previous run.
    std::error_code ec;
    do
    {
      dump.path = details::NumberedPath(options.filepath, ++dump_counter);
    } while(std::filesystem::exists(dump.path, ec));

    // mcap::FileWriter ignores fwrite() and fclose() failures (disk full, I/O
    // errors); a stream reports them.
    std::ofstream file(dump.path, std::ios::binary | std::ios::trunc);
    if(!file)
    {
      throw std::runtime_error("cannot open file: " + std::string(std::strerror(errno)));
    }
    mcap::StreamWriter stream(file);
    mcap::McapWriter writer;
    mcap::McapWriterOptions writer_options(mcap_encoding::kEncoding);
    writer_options.compression =
        options.compression ? mcap::Compression::Zstd : mcap::Compression::None;
    writer.open(stream, writer_options);

    struct Channel
    {
      mcap::ChannelId id;
      uint32_t next_sequence;
    };
    std::unordered_map<uint64_t, Channel> channels;
    std::vector<uint8_t> scratch;
    for(size_t pos = 0; pos < dump_size;)
    {
      RecordHeader header;
      std::memcpy(&header, dump_buffer.data() + pos, sizeof(header));
      const uint8_t* mask = dump_buffer.data() + pos + sizeof(header);
      const uint8_t* payload = mask + header.mask_size;
      pos += header.recordSize();
      auto it = channels.find(header.schema_hash);
      if(it == channels.end())
      {
        const Schema* schema = nullptr;
        {
          std::scoped_lock lock(schema_mutex);
          auto found = schemas.find(header.schema_hash);
          schema = (found == schemas.end()) ? nullptr : &found->second;
        }
        if(!schema)
        {
          continue;  // cannot happen: onSchema() precedes the snapshots
        }
        it = channels
                 .emplace(header.schema_hash,
                          Channel{ mcap_encoding::AddChannel(writer, *schema), 1 })
                 .first;
      }
      const nanoseconds timestamp(header.timestamp);
      const auto status = mcap_encoding::WriteMessage(
          writer, it->second.id, it->second.next_sequence++, timestamp,
          { mask, header.mask_size }, { payload, header.payload_size }, scratch);
      if(!status.ok())
      {
        throw std::runtime_error("MCAP write failed: " + status.message);
      }
      dump.first_message =
          (dump.messages == 0) ? timestamp : std::min(dump.first_message, timestamp);
      dump.last_message =
          (dump.messages == 0) ? timestamp : std::max(dump.last_message, timestamp);
      ++dump.messages;
    }
    writer.close();  // flushes the stream
    file.close();
    if(!file)
    {
      // The partial file is kept: `mcap recover` can salvage what it holds.
      throw std::runtime_error("write failed: " + std::string(std::strerror(errno)));
    }
    dump.ok = true;
  }
  catch(const std::exception& e)
  {
    try
    {
      dump.error = e.what();
    }
    catch(...)
    {}  // bad_alloc while reporting: ok stays false
  }
}

//--------------------------------------------------

MCAPRingSink::MCAPRingSink(MCAPRingOptions options)
{
  if(options.capacity_bytes == 0)
  {
    throw std::invalid_argument("MCAPRingSink: capacity_bytes must be > 0");
  }
  if(options.window.count() < 0)
  {
    throw std::invalid_argument("MCAPRingSink: window must not be negative");
  }
  _p = std::make_shared<Pimpl>(std::move(options));
  _p->writer_thread = std::thread([p = _p] { p->writerLoop(); });
}

MCAPRingSink::~MCAPRingSink()
{
  {
    std::scoped_lock lock(_p->writer_mutex);
    _p->writer_stop = true;
  }
  _p->writer_cv.notify_all();
  if(_p->writer_thread.get_id() == std::this_thread::get_id())
  {
    // Destroyed from the dump callback: joining would deadlock. The writer
    // thread holds its own reference to the Pimpl and exits once the callback
    // returns.
    _p->writer_thread.detach();
  }
  else
  {
    _p->writer_thread.join();
  }
}

bool MCAPRingSink::requestDump(std::chrono::nanoseconds post_trigger)
{
  const auto delay =
      static_cast<uint64_t>(std::max<nanoseconds::rep>(post_trigger.count(), 0));
  uint64_t expected = 0;
  return _p->request.compare_exchange_strong(
      expected, kPending | std::min(delay, kMaxDelay), std::memory_order_acq_rel,
      std::memory_order_relaxed);
}

bool MCAPRingSink::dumpRequested() const
{
  return _p->request.load(std::memory_order_acquire) != 0;
}

void MCAPRingSink::onSchema(const Schema& schema)
{
  std::scoped_lock lock(_p->schema_mutex);
  _p->schemas.try_emplace(schema.hash, schema);
}

void MCAPRingSink::onSnapshot(const SnapshotRef& ref)
{
  const Snapshot& snapshot = *ref;
  auto& p = *_p;
  std::scoped_lock lock(p.ring_mutex);
  p.has_snapshot = true;
  p.max_timestamp = std::max(p.max_timestamp, snapshot.timestamp);

  if(p.phase == Pimpl::Phase::Idle)
  {
    const uint64_t word = p.request.load(std::memory_order_acquire);
    if(word & kPending)
    {
      p.phase = Pimpl::Phase::Collecting;
      p.trigger_time = snapshot.timestamp;
      p.dump_start = saturatingSub(snapshot.timestamp, p.options.window);
      p.dump_end = saturatingAdd(snapshot.timestamp, nanoseconds(word & kMaxDelay));
    }
  }
  // Complete only once a snapshot is past the end: another channel may still
  // deliver snapshots stamped at the end itself.
  if(p.phase == Pimpl::Phase::Collecting && snapshot.timestamp > p.dump_end)
  {
    p.phase = Pimpl::Phase::Ready;
  }
  p.store(snapshot);
  if(p.phase == Pimpl::Phase::Ready)
  {
    p.tryHandOff();  // writer busy: retried at the next snapshot
  }
}

bool MCAPRingSink::flushPendingDump()
{
  auto& p = *_p;
  p.throwIfWriterThread("flushPendingDump()");
  bool handed_off = false;
  while(!handed_off)
  {
    waitForWriter();  // never wait for the writer while holding ring_mutex
    std::scoped_lock lock(p.ring_mutex);
    if(p.phase == Pimpl::Phase::Idle)
    {
      if(!p.has_snapshot || !(p.request.load(std::memory_order_acquire) & kPending))
      {
        break;  // nothing to write
      }
      p.trigger_time = p.max_timestamp;
      p.dump_start = saturatingSub(p.max_timestamp, p.options.window);
      p.dump_end = p.max_timestamp;
    }
    p.dump_end = std::min(p.dump_end, p.max_timestamp);
    p.phase = Pimpl::Phase::Ready;
    handed_off = p.tryHandOff();
  }
  waitForWriter();  // our dump, or one handed off earlier
  return handed_off;
}

void MCAPRingSink::waitForWriter()
{
  _p->throwIfWriterThread("waitForWriter()");
  std::unique_lock lock(_p->writer_mutex);
  _p->writer_cv.wait(lock, [this] { return !_p->writer_busy; });
}

void MCAPRingSink::setDumpCallback(std::function<void(const MCAPRingDump&)> callback)
{
  std::scoped_lock lock(_p->writer_mutex);
  _p->callback = std::move(callback);
}

MCAPRingStats MCAPRingSink::stats() const
{
  MCAPRingStats stats;
  {
    std::scoped_lock lock(_p->ring_mutex);
    stats.writer_busy_retries = _p->writer_busy_retries;
    stats.evicted_by_capacity = _p->evicted_by_capacity;
    stats.dropped_oversize = _p->dropped_oversize;
    stats.stored_snapshots = _p->ring.count();
    stats.stored_bytes = _p->ring.used();
  }
  std::scoped_lock lock(_p->writer_mutex);
  stats.dumps_written = _p->dumps_written;
  stats.dumps_failed = _p->dumps_failed;
  return stats;
}

}  // namespace DataTamer
