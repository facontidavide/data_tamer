#include "data_tamer/data_tamer.hpp"
#include "data_tamer/channel.hpp"
#include "data_tamer/data_sink.hpp"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace DataTamer
{

namespace
{
// On a new channel: sizes resolved and validated by setChannelDefaults().
void ApplyDefaults(LogChannel& channel, const ChannelDefaults::Sizes& sizes)
{
  if(sizes.pool_slots != 0)
  {
    channel.setPoolCapacity(sizes.pool_slots);
  }
  if(sizes.payload_bytes != 0)
  {
    channel.setPayloadCapacity(sizes.payload_bytes);
  }
}
}  // namespace

struct ChannelsRegistry::Pimpl
{
  std::unordered_map<std::string, std::shared_ptr<LogChannel>> channels;
  std::unordered_set<std::shared_ptr<SinkWorker>> default_sinks;
  ChannelDefaults::Sizes channel_sizes;
  std::mutex mutex;
};

ChannelsRegistry::ChannelsRegistry() : _p(new Pimpl) {}

ChannelsRegistry::~ChannelsRegistry() {}

ChannelsRegistry& ChannelsRegistry::Global()
{
  static ChannelsRegistry obj;
  return obj;
}

void ChannelsRegistry::addDefaultSink(std::shared_ptr<SinkWorker> sink)
{
  if(!sink)
  {
    throw std::invalid_argument("addDefaultSink: null sink");
  }
  // A default sink from here on: getChannel() attaches it to the channels it
  // creates meanwhile. The existing ones are attached without the registry
  // lock, since a prepared channel announces its schema to the sink.
  bool inserted = false;
  std::vector<std::shared_ptr<LogChannel>> channels;
  {
    std::scoped_lock lk(_p->mutex);
    inserted = _p->default_sinks.insert(sink).second;
    channels.reserve(_p->channels.size());
    for(const auto& [name, channel] : _p->channels)
    {
      channels.push_back(channel);
    }
  }
  std::vector<LogChannel*> attached;
  attached.reserve(channels.size());  // the undo list must not fail to grow
  try
  {
    for(const auto& channel : channels)
    {
      if(channel->addDataSink(sink))  // false: held already, e.g. by getChannel()
      {
        attached.push_back(channel.get());
      }
    }
  }
  catch(...)
  {
    for(LogChannel* channel : attached)
    {
      channel->removeDataSink(sink);
    }
    if(inserted)
    {
      std::scoped_lock lk(_p->mutex);
      _p->default_sinks.erase(sink);
    }
    throw;
  }
}

void ChannelsRegistry::setChannelDefaults(const ChannelDefaults& defaults)
{
  const auto sizes = defaults.resolve();
  std::scoped_lock lk(_p->mutex);
  _p->channel_sizes = sizes;
}

std::shared_ptr<LogChannel> ChannelsRegistry::getChannel(std::string const& channel_name)
{
  std::scoped_lock lk(_p->mutex);
  auto it = _p->channels.find(channel_name);
  if(it == _p->channels.end())
  {
    auto new_channel = LogChannel::create(channel_name);
    ApplyDefaults(*new_channel, _p->channel_sizes);
    for(auto const& sink : _p->default_sinks)
    {
      new_channel->addDataSink(sink);
    }
    _p->channels.insert({ channel_name, new_channel });
    return new_channel;
  }
  return it->second;
}

void ChannelsRegistry::stopAll()
{
  std::vector<std::shared_ptr<SinkWorker>> sinks;
  {
    std::scoped_lock lk(_p->mutex);
    sinks.assign(_p->default_sinks.begin(), _p->default_sinks.end());
    for(const auto& [name, channel] : _p->channels)
    {
      for(auto& sink : channel->dataSinks())
      {
        sinks.push_back(std::move(sink));
      }
    }
  }
  std::sort(sinks.begin(), sinks.end());
  sinks.erase(std::unique(sinks.begin(), sinks.end()), sinks.end());
  // stop() delivers, joins and runs onStop(): never under the registry lock.
  for(const auto& sink : sinks)
  {
    sink->stop();
  }
}

void ChannelsRegistry::clear()
{
  // Destroying channels and sinks can block (they drain and join): never do
  // that while holding the registry lock.
  decltype(_p->channels) channels;
  decltype(_p->default_sinks) sinks;
  {
    std::scoped_lock lk(_p->mutex);
    channels.swap(_p->channels);
    sinks.swap(_p->default_sinks);
    _p->channel_sizes = {};
  }
}

}  // namespace DataTamer
