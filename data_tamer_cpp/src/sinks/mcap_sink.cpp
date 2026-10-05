#include "data_tamer/sinks/mcap_sink.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>

#ifndef USING_ROS2
#define MCAP_IMPLEMENTATION
#endif

// After MCAP_IMPLEMENTATION: this includes <mcap/writer.hpp>.
#include "data_tamer/sinks/mcap_encoding.hpp"
#include <mcap/reader.hpp>  // its implementation is compiled here, as before

namespace DataTamer
{

namespace details
{
std::string NumberedPath(const std::string& path, size_t number)
{
  const auto suffix = "_" + std::to_string(number);
  const auto slash = path.find_last_of("/\\");
  const size_t name_begin = (slash == std::string::npos) ? 0 : slash + 1;
  // Skip the leading dots of a hidden file (".foo"): they are not an extension.
  const size_t stem_begin = path.find_first_not_of('.', name_begin);
  if(stem_begin == std::string::npos)
  {
    return path + suffix;
  }
  // The extension is the trailing run of purely alphabetic dot-segments
  // (".tamer.mcap"); "1.2" or "v2" belong to the stem.
  const auto alphabetic = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
  };
  size_t extension_begin = path.size();
  size_t segment_end = path.size();
  while(true)
  {
    const size_t dot = path.rfind('.', segment_end - 1);
    if(dot == std::string::npos || dot <= stem_begin)
    {
      break;
    }
    const auto first = path.begin() + static_cast<std::ptrdiff_t>(dot + 1);
    const auto last = path.begin() + static_cast<std::ptrdiff_t>(segment_end);
    if(first == last || !std::all_of(first, last, alphabetic))
    {
      break;
    }
    extension_begin = dot;
    segment_end = dot;
  }
  return path.substr(0, extension_begin) + suffix + path.substr(extension_begin);
}
}  // namespace details

struct MCAPSink::Pimpl
{
  std::string filepath;
  bool compression = false;
  std::unique_ptr<mcap::McapWriter> writer;

  struct ChannelInfo
  {
    mcap::ChannelId id = 0;
    // Sequence number of the next message, per MCAP channel and per file.
    uint32_t next_sequence = 1;
  };
  std::unordered_map<uint64_t, ChannelInfo> hash_to_channel;
  std::unordered_map<uint64_t, Schema> schemas;

  bool create_file_on_reset = true;
  std::string original_filepath;
  size_t file_reset_counter = 1;

  std::chrono::seconds reset_time = std::chrono::seconds(60 * 10);
  std::chrono::system_clock::time_point start_time;

  std::vector<uint8_t> message_body;  // reused, see mcap_encoding::WriteMessage
  bool forced_stop_recording = false;
  std::recursive_mutex mutex;
};

MCAPSink::MCAPSink(const std::string& filepath, bool do_compression)
  : _p(std::make_unique<Pimpl>())
{
  _p->filepath = filepath;
  _p->compression = do_compression;
  _p->original_filepath = filepath;
  openFile(_p->filepath, do_compression);
}

void DataTamer::MCAPSink::openFile(std::string const& filepath, bool do_compression)
{
  std::scoped_lock lk(_p->mutex);
  // Open the new file first: if that fails the current recording stays intact.
  auto writer = std::make_unique<mcap::McapWriter>();
  mcap::McapWriterOptions options(mcap_encoding::kEncoding);
  options.compression =
      do_compression ? mcap::Compression::Zstd : mcap::Compression::None;
  const auto status = writer->open(filepath, options);
  if(!status.ok())
  {
    throw std::runtime_error("Failed to open MCAP file for writing: " + status.message);
  }
  _p->writer = std::move(writer);  // closes the previous file
  _p->filepath = filepath;
  _p->compression = do_compression;
  _p->start_time = std::chrono::system_clock::now();
  _p->hash_to_channel.clear();
}

MCAPSink::~MCAPSink() = default;

void MCAPSink::onSchema(Schema const& schema)
{
  std::scoped_lock lk(_p->mutex);
  _p->schemas[schema.hash] = schema;
  auto it = _p->hash_to_channel.find(schema.hash);
  if(it != _p->hash_to_channel.end() || !_p->writer)  // stopped: re-added on restart
  {
    return;
  }

  _p->hash_to_channel[schema.hash].id = mcap_encoding::AddChannel(*_p->writer, schema);
}

void MCAPSink::onSnapshot(const SnapshotRef& ref)
{
  std::scoped_lock lk(_p->mutex);
  if(_p->forced_stop_recording)
  {
    return;
  }
  const Snapshot& snapshot = *ref;
  auto& channel = _p->hash_to_channel.at(snapshot.schema_hash);
  const auto status = mcap_encoding::WriteSnapshot(
      *_p->writer, channel.id, channel.next_sequence++, snapshot, _p->message_body);
  if(!status.ok())
  {
    throw std::runtime_error("MCAP write failed: " + status.message);
  }

  // If reset_time is exceeded, continue in a new numbered file (or truncate
  // the current one, if create_file_on_reset was disabled).
  if(_p->reset_time != std::chrono::seconds(0) &&
     std::chrono::system_clock::now() - _p->start_time > _p->reset_time)
  {
    if(_p->create_file_on_reset)
    {
      // The original path with "_<n>" inserted. Skip the names that exist
      // already (e.g. from a previous run): opening a file truncates it.
      size_t counter = _p->file_reset_counter;
      std::string next;
      std::error_code ec;
      do
      {
        next = details::NumberedPath(_p->original_filepath, counter++);
      } while(std::filesystem::exists(next, ec));
      restartRecordingImpl(next, _p->compression, false);
      _p->file_reset_counter = counter;  // only once the file is open
    }
    else
    {
      restartRecordingImpl(_p->filepath, _p->compression, false);
    }
  }
}

void MCAPSink::setMaxTimeBeforeReset(std::chrono::seconds reset_time)
{
  std::scoped_lock lk(_p->mutex);  // read by the worker in onSnapshot
  _p->reset_time = reset_time;
}

void MCAPSink::setCreateNewFileOnReset(bool create_file_on_reset)
{
  std::scoped_lock lk(_p->mutex);
  _p->create_file_on_reset = create_file_on_reset;
}

void MCAPSink::stopRecording()
{
  std::scoped_lock lk(_p->mutex);
  _p->forced_stop_recording = true;
  if(_p->writer)  // idempotent
  {
    _p->writer->close();
    _p->writer.reset();
  }
}

void MCAPSink::restartRecording(const std::string& filepath, bool do_compression)
{
  restartRecordingImpl(filepath, do_compression, true);
}

void MCAPSink::restartRecordingImpl(const std::string& filepath, bool do_compression,
                                    bool new_file)
{
  std::scoped_lock lk(_p->mutex);
  openFile(filepath, do_compression);  // throws: the current recording is kept
  if(new_file)
  {
    // if this was called by a user, we need to change the filepath that we will increment when reset
    _p->file_reset_counter = 1;
    _p->original_filepath = filepath;
  }

  // rebuild the channels
  for(auto const& [hash, schema] : _p->schemas)
  {
    onSchema(schema);
  }

  if(new_file)
  {
    _p->forced_stop_recording = false;
  }
}

}  // namespace DataTamer
