#include "data_tamer/sinks/mcap_sink.hpp"
#include "data_tamer/contrib/SerializeMe.hpp"

#include <chrono>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>

#ifndef USING_ROS2
#define MCAP_IMPLEMENTATION
#endif

#include <mcap/writer.hpp>
#include <mcap/reader.hpp>

#if defined __has_include && __has_include("boost/container/small_vector.hpp")
#include <boost/container/small_vector.hpp>

namespace SerializeMe
{
template <size_t N>
void SerializeIntoBuffer(SpanBytes& buffer,
                         boost::container::small_vector<uint8_t, N> const& value)
{
  SerializeMe::SerializeIntoBuffer(buffer, uint32_t(value.size()));
  std::memcpy(buffer.data(), value.data(), value.size());
  buffer.trimFront(value.size());
}
}  // end namespace SerializeMe

#endif

namespace DataTamer
{

static constexpr char const* kDataTamer = "data_tamer";

namespace
{
// "log.mcap" -> "log_3.mcap"; a path without an extension gets the suffix appended.
std::string NumberedPath(const std::string& path, size_t number)
{
  const auto suffix = "_" + std::to_string(number);
  const auto slash = path.find_last_of("/\\");
  const auto dot = path.rfind('.');
  if(dot == std::string::npos || (slash != std::string::npos && dot < slash))
  {
    return path + suffix;
  }
  return path.substr(0, dot) + suffix + path.substr(dot);
}
}  // namespace

struct MCAPSink::Pimpl
{
  std::string filepath;
  bool compression = false;
  std::unique_ptr<mcap::McapWriter> writer;

  std::unordered_map<uint64_t, uint16_t> hash_to_channel_id;
  std::unordered_map<std::string, Schema> schemas;

  bool create_file_on_reset = false;
  std::string original_filepath;
  size_t file_reset_counter = 1;

  std::chrono::seconds reset_time = std::chrono::seconds(60 * 10);
  std::chrono::system_clock::time_point start_time;

  std::vector<uint8_t> merged_payload;
  bool forced_stop_recording = false;
  std::recursive_mutex mutex;
};

MCAPSink::MCAPSink(const std::string& filepath, bool do_compression,
                   size_t queue_capacity)
  : DataSinkBase(queue_capacity), _p(std::make_unique<Pimpl>())
{
  _p->filepath = filepath;
  _p->compression = do_compression;
  _p->original_filepath = filepath;
  openFile(_p->filepath);
}

void DataTamer::MCAPSink::openFile(std::string const& filepath)
{
  std::scoped_lock lk(_p->mutex);
  // Open the new file first: if that fails the current recording stays intact.
  auto writer = std::make_unique<mcap::McapWriter>();
  mcap::McapWriterOptions options(kDataTamer);
  options.compression =
      _p->compression ? mcap::Compression::Zstd : mcap::Compression::None;
  const auto status = writer->open(filepath, options);
  if(!status.ok())
  {
    throw std::runtime_error("Failed to open MCAP file for writing: " + status.message);
  }
  _p->writer = std::move(writer);  // closes the previous file
  _p->start_time = std::chrono::system_clock::now();
  _p->hash_to_channel_id.clear();
}

MCAPSink::~MCAPSink()
{
  stopThread();
  std::scoped_lock lk(_p->mutex);
}

void MCAPSink::addChannel(std::string const& channel_name, Schema const& schema)
{
  std::scoped_lock lk(_p->mutex);
  _p->schemas[channel_name] = schema;
  auto it = _p->hash_to_channel_id.find(schema.hash);
  if(it != _p->hash_to_channel_id.end() || !_p->writer)  // stopped: re-added on restart
  {
    return;
  }

  std::stringstream ss;
  ss << schema;
  std::string schema_str = ss.str();

  auto const schema_name = channel_name + "::" + std::to_string(schema.hash);

  // Register a Schema
  mcap::Schema mcap_schema(schema_name, kDataTamer, schema_str);
  _p->writer->addSchema(mcap_schema);

  // Register a Channel
  mcap::Channel publisher(channel_name, kDataTamer, mcap_schema.id);
  _p->writer->addChannel(publisher);
  _p->hash_to_channel_id[schema.hash] = publisher.id;
}

bool MCAPSink::storeSnapshot(const Snapshot& snapshot)
{
  std::scoped_lock lk(_p->mutex);
  if(_p->forced_stop_recording)
  {
    return false;
  }
  // the payload must contain both the ActiveMask and the other data
  auto& merged_payload = _p->merged_payload;
  const auto size_mask = snapshot.active_mask.size();
  const auto size_data = snapshot.payload.size();

  merged_payload.resize(size_mask + size_data + sizeof(uint32_t) * 2);
  SerializeMe::SpanBytes buffer(merged_payload);
  SerializeMe::SerializeIntoBuffer(buffer, snapshot.active_mask);
  SerializeMe::SerializeIntoBuffer(buffer, snapshot.payload);

  // Write our message
  mcap::Message msg;
  msg.channelId = _p->hash_to_channel_id.at(snapshot.schema_hash);
  msg.sequence = 1;  // Optional
  // Timestamp requires nanosecond
  msg.logTime = mcap::Timestamp(snapshot.timestamp.count());
  msg.publishTime = msg.logTime;
  msg.data = reinterpret_cast<std::byte const*>(merged_payload.data());  // NOLINT
  msg.dataSize = merged_payload.size();
  const bool written = _p->writer->write(msg).ok();

  // If reset_time is exceeded, we want to overwrite the current file.
  // Better than filling the disk, if you forgot to stop the application.
  if(_p->reset_time != std::chrono::seconds(0) &&
     std::chrono::system_clock::now() - _p->start_time > _p->reset_time)
  {
    if(_p->create_file_on_reset)
    {
      // change the current filepath to the original with "_[# resets]"" appended
      _p->filepath = NumberedPath(_p->original_filepath, _p->file_reset_counter);
      ++_p->file_reset_counter;
    }
    restartRecordingImpl(_p->filepath, _p->compression, false);
  }
  return written;
}

void MCAPSink::setMaxTimeBeforeReset(std::chrono::seconds reset_time)
{
  std::scoped_lock lk(_p->mutex);  // read by the worker in storeSnapshot
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
  if(_p->writer)  // idempotent: finishQueueAndStop() may follow stopRecording()
  {
    _p->writer->close();
    _p->writer.reset();
  }
}

void MCAPSink::finishQueueAndStop()
{
  // stop accepting new snapshots
  stopAcceptingSnapshots();

  // finish any that are queued
  processQueuedSnapshots();

  // now stop the recording as normal
  stopRecording();
}

void MCAPSink::restartRecording(const std::string& filepath, bool do_compression)
{
  restartRecordingImpl(filepath, do_compression, true);
}

void MCAPSink::restartRecordingImpl(const std::string& filepath, bool do_compression,
                                    bool new_file)
{
  std::scoped_lock lk(_p->mutex);
  if(new_file)
  {
    // if this was called by a user, we need to change the filepath that we will increment when reset
    _p->file_reset_counter = 1;
    _p->original_filepath = filepath;
  }
  _p->filepath = filepath;
  _p->compression = do_compression;
  openFile(_p->filepath);

  // rebuild the channels
  for(auto const& [name, schema] : _p->schemas)
  {
    addChannel(name, schema);
  }

  if(new_file)
  {
    _p->forced_stop_recording = false;
    startAcceptingSnapshots();
  }
}

}  // namespace DataTamer
