#pragma once

#include "data_tamer/contrib/SerializeMe.hpp"
#include "data_tamer/data_sink.hpp"
#include "data_tamer/types.hpp"

// See "Building against it" below: do not define MCAP_IMPLEMENTATION.
// (Internal: mcap_sink.cpp, which does, defines it before including this header.)
#include <mcap/writer.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

/**
 * The MCAP encoding of docs/wire_format.md (section 4.1), shared by MCAPSink and
 * by any other code that writes data_tamer MCAP files, for example a sink that
 * stores snapshots and writes them later.
 *
 *   mcap::McapWriter writer;
 *   if(!writer.open(path, mcap::McapWriterOptions(mcap_encoding::kEncoding)).ok())
 *     return false;
 *   const auto id = mcap_encoding::AddChannel(writer, schema);
 *   std::vector<uint8_t> scratch;  // reused: no allocation once large enough
 *   uint32_t sequence = 1;
 *   for(const Snapshot& snapshot : stored)
 *   {
 *     if(!mcap_encoding::WriteSnapshot(writer, id, sequence++, snapshot, scratch).ok())
 *       return false;
 *   }
 *   writer.close();
 *
 * Sequence numbers are provided by the caller: MCAPSink counts 1, 2, 3, ... per
 * MCAP channel and file, and a writer of stored data should do the same (or
 * keep the numbers it recorded), so that readers can detect gaps.
 *
 * Building against it. The functions are inline, but they are not standalone:
 * link data_tamer::data_tamer (AddChannel uses the library's operator<<(Schema)),
 * and use the MCAP headers data_tamer was built with, so that the McapWriter
 * your code sees is the one the library compiled:
 * - Outside ROS 2, data_tamer::data_tamer carries the include path of its MCAP
 *   headers (the bundled copy is installed with it), and libdata_tamer contains
 *   the MCAP implementation. Do NOT define MCAP_IMPLEMENTATION in your code:
 *   the definitions would be duplicated. Do not mix in another MCAP version.
 * - Under ROS 2, MCAP comes from mcap_vendor, which is not compiled into
 *   libdata_tamer: link mcap_vendor::mcap as well.
 */
namespace DataTamer::mcap_encoding
{

/// Writer profile, schema encoding and message encoding.
inline constexpr char kEncoding[] = "data_tamer";

/// Read-only bytes: a std::vector<uint8_t> converts implicitly, or use
/// SerializeMe::SpanBytesConst(pointer, size).
using ByteSpan = SerializeMe::SpanBytesConst;

/// Name of the MCAP schema record of `schema`: "<channel_name>::<hash>".
inline std::string SchemaName(const Schema& schema)
{
  return schema.channel_name + "::" + std::to_string(schema.hash);
}

/// Register the MCAP schema and channel records of `schema` and return the
/// channel id to pass to WriteMessage()/WriteSnapshot(). Call it once per schema
/// and file (MCAP ids are per file). The schema record holds the line format of
/// docs/wire_format.md section 2 (operator<< / ToStr), never ToYaml: MCAP files
/// keep the line format.
inline mcap::ChannelId AddChannel(mcap::McapWriter& writer, const Schema& schema)
{
  std::ostringstream ss;
  ss << schema;
  mcap::Schema mcap_schema(SchemaName(schema), kEncoding, ss.str());
  writer.addSchema(mcap_schema);
  mcap::Channel channel(schema.channel_name, kEncoding, mcap_schema.id);
  writer.addChannel(channel);
  return channel.id;
}

/// Size of a message body holding `mask_size` mask bytes and `payload_size`
/// payload bytes.
inline size_t MessageBodySize(size_t mask_size, size_t payload_size)
{
  return 2 * sizeof(uint32_t) + mask_size + payload_size;
}

/// Write the message body `u32 mask_len, mask, u32 payload_len, payload`
/// (lengths little-endian) into `body`, resized to fit. Does not allocate when
/// `body` already has the capacity. `mask` and `payload` must not point into
/// `body`: it is resized and overwritten. Throws std::length_error if a length
/// does not fit in 32 bits.
inline void EncodeMessageBody(ByteSpan mask, ByteSpan payload, std::vector<uint8_t>& body)
{
  constexpr size_t kMaxLength = std::numeric_limits<uint32_t>::max();
  if(mask.size() > kMaxLength || payload.size() > kMaxLength)
  {
    throw std::length_error("data_tamer MCAP message: mask or payload too large");
  }
  body.resize(MessageBodySize(mask.size(), payload.size()));
  uint8_t* out = body.data();
  const auto put = [&out](ByteSpan bytes) {
    const auto length = static_cast<uint32_t>(bytes.size());
    for(size_t i = 0; i < sizeof(uint32_t); ++i)
    {
      *out++ = static_cast<uint8_t>(length >> (8 * i));
    }
    if(bytes.size() > 0)
    {
      std::copy(bytes.data(), bytes.data() + bytes.size(), out);
      out += bytes.size();
    }
  };
  put(mask);
  put(payload);
}

/// Write one message from its parts: `timestamp` (nanoseconds since the epoch)
/// becomes logTime and publishTime. `scratch` holds the encoded body; reuse it
/// across calls to avoid allocations (it must not hold `mask` or `payload`).
/// Returns the writer's status.
inline mcap::Status WriteMessage(mcap::McapWriter& writer, mcap::ChannelId channel_id,
                                 uint32_t sequence, std::chrono::nanoseconds timestamp,
                                 ByteSpan mask, ByteSpan payload,
                                 std::vector<uint8_t>& scratch)
{
  EncodeMessageBody(mask, payload, scratch);
  mcap::Message msg;
  msg.channelId = channel_id;
  msg.sequence = sequence;
  msg.logTime = mcap::Timestamp(timestamp.count());
  msg.publishTime = msg.logTime;
  msg.data = reinterpret_cast<const std::byte*>(scratch.data());  // NOLINT
  msg.dataSize = scratch.size();
  return writer.write(msg);
}

/// WriteMessage() for a Snapshot, as received by DataSink::onSnapshot()
/// (`*ref`) or stored by the application.
inline mcap::Status WriteSnapshot(mcap::McapWriter& writer, mcap::ChannelId channel_id,
                                  uint32_t sequence, const Snapshot& snapshot,
                                  std::vector<uint8_t>& scratch)
{
  return WriteMessage(writer, channel_id, sequence, snapshot.timestamp,
                      snapshot.active_mask, snapshot.payload, scratch);
}

}  // namespace DataTamer::mcap_encoding
