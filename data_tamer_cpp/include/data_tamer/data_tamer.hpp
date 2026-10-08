#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>

#include "data_tamer/fwd.hpp"

// ChannelsRegistry only names LogChannel and SinkWorker through std::shared_ptr.
// Include data_tamer/channel.hpp to use a channel, data_tamer/data_sink.hpp for a sink.

namespace DataTamer
{

/**
 * @brief Settings that ChannelsRegistry::getChannel() applies to the channels it
 * creates, before attaching the default sinks. Zero keeps the library default. Set
 * the pool as a count or in time, not both.
 *
 * ABI: passed by value, so a change of its layout requires a SONAME bump.
 */
struct ChannelDefaults
{
  /// LogChannel::setPoolCapacity(count): pool slots per channel.
  size_t pool_capacity = 0;
  /// LogChannel::setPoolCapacity(stall_tolerance, snapshot_period): the pool
  /// sized in time. Set both or neither.
  std::chrono::nanoseconds pool_stall_tolerance{ 0 };
  std::chrono::nanoseconds pool_snapshot_period{ 0 };
  /// LogChannel::setPayloadCapacity(bytes): minimum payload bytes per slot.
  size_t payload_capacity = 0;

  /// The settings as LogChannel sizes; zero leaves the library default.
  struct Sizes
  {
    /// For LogChannel::setPoolCapacity(count).
    size_t pool_slots = 0;
    /// For LogChannel::setPayloadCapacity(bytes).
    size_t payload_bytes = 0;
  };

  /// Validates the settings and converts the pool in time to a slot count. Throws
  /// std::invalid_argument if both pool forms are set or a duration is not positive,
  /// and std::length_error if a size is too large for the LogChannel setters.
  [[nodiscard]] Sizes resolve() const;
};

/// Creates channels by name and gives each the same default settings and default sinks.
/// Members may be called from any thread.
class ChannelsRegistry
{
public:
  ChannelsRegistry();

  // Defined in the .cpp, where Pimpl is complete.
  ~ChannelsRegistry();

  /// The process-wide registry.
  static ChannelsRegistry& Global();

  /// Attaches the sink to every channel of the registry: those that exist (a started
  /// channel sends its schema to the sink at once, see LogChannel::addDataSink()) and
  /// those getChannel() creates later. If a channel refuses it (eight sinks already,
  /// or onSchema() throws), the call is undone, the sink is not a default sink and
  /// the exception propagates.
  void addDefaultSink(std::shared_ptr<SinkWorker> sink);

  /// Sets the defaults for the channels getChannel() creates after this call; existing
  /// channels are not changed. Throws what ChannelDefaults::resolve() throws, and then
  /// keeps the previous defaults.
  void setChannelDefaults(const ChannelDefaults& defaults);

  /// Returns the channel with this name, creating it on first use with the channel
  /// defaults and the default sinks. The registry keeps it until clear().
  [[nodiscard]] std::shared_ptr<LogChannel> getChannel(std::string const& channel_name);

  /// Calls SinkWorker::stop() once on each sink the registry reaches: the default sinks
  /// and the sinks attached to channels created by getChannel(). Sinks of channels made
  /// with LogChannel::create() are reached only if they are default sinks. The stops
  /// run on the calling thread in no particular order; each delivers what is queued,
  /// then finishes the sink (DataSink::onStop()). The sinks stay attached: restart one
  /// with SinkWorker::start(). Never call it from a sink callback.
  void stopAll();

  /// Drops the registry's references to all channels and default sinks, and resets the
  /// channel defaults. Dropping the last reference to a worker stops it, which can block.
  void clear();

private:
  struct Pimpl;
  std::unique_ptr<Pimpl> _p;
};

}  // namespace DataTamer
