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
 * @brief Settings ChannelsRegistry::getChannel() applies to every channel it
 * creates, before attaching the default sinks. Zero leaves a setting at the
 * library default. Set the pool either as a count or in time, not both.
 *
 * ABI: passed by value across the library boundary; a change of its layout
 * requires a SONAME bump.
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

  /// Validates the settings and resolves the pool in time to a slot count,
  /// ceil(stall_tolerance / snapshot_period). Throws std::invalid_argument
  /// when both pool forms are set, or a duration is set and either duration is
  /// not positive, and std::length_error when a size is too large for the
  /// LogChannel setters.
  [[nodiscard]] Sizes resolve() const;
};

class ChannelsRegistry
{
public:
  ChannelsRegistry();

  // the Pimpl idiom does not allow a default destructor
  ~ChannelsRegistry();

  // global instance (similar to singleton)
  static ChannelsRegistry& Global();

  /// Attach this sink to every channel of the registry: the ones that exist
  /// (a prepared channel announces its schema to the sink at once, see
  /// LogChannel::addDataSink) and the ones getChannel() creates later.
  /// A sink a channel holds already is not attached twice. If a channel
  /// refuses the sink (eight sinks already, onSchema() throws), the sink is
  /// detached again from the channels this call attached it to, it does not
  /// become a default sink, and the exception propagates. A channel that
  /// getChannel() creates while the call runs gets the sink as a default sink
  /// and keeps it.
  void addDefaultSink(std::shared_ptr<SinkWorker> sink);

  /// Settings for the channels getChannel() creates from now on. Channels that
  /// exist already are not changed: their owner may have configured them, and
  /// pool and payload sizes freeze at prepare().
  /// Throws what ChannelDefaults::resolve() throws (checked here, not at
  /// getChannel()); the previous defaults are then kept.
  void setChannelDefaults(const ChannelDefaults& defaults);

  /// Create a new channel or get a previously created one. A new channel gets
  /// the channel defaults, then the default sinks.
  [[nodiscard]] std::shared_ptr<LogChannel> getChannel(std::string const& channel_name);

  /// SinkWorker::stop() on every sink the registry reaches, each once even
  /// when several channels share it: the default sinks, and every sink
  /// attached at the time of the call to a channel created by getChannel(),
  /// whether by addDefaultSink() or by LogChannel::addDataSink(). Sinks of
  /// channels made with LogChannel::create() are reached only if they are
  /// default sinks. The stops run on the calling thread, in no particular
  /// order; each delivers what is queued, then finishes the sink
  /// (DataSink::onStop()). The sinks stay attached: restart one with
  /// SinkWorker::start(). Never call it from a sink callback.
  void stopAll();

  /// Remove all channels and stored sinks, and reset the channel defaults.
  void clear();

private:
  struct Pimpl;
  std::unique_ptr<Pimpl> _p;
};

}  // namespace DataTamer
