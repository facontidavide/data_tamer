#include "data_tamer/channel.hpp"
#include "data_tamer/data_tamer.hpp"
#include "data_tamer/sinks/dummy_sink.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <vector>

using namespace DataTamer;

// A channel holds eight sinks, so a ninth default sink could never reach any channel.
// It is refused when added; accepted, it made every later getChannel() throw.
TEST(ChannelsRegistry, RefusesANinthDefaultSinkBeforeAnyChannelExists)
{
  ChannelsRegistry registry;
  std::vector<std::shared_ptr<SinkWorker>> sinks;
  for(size_t i = 0; i < LogChannel::kMaxSinks; ++i)
  {
    sinks.push_back(DummySink::create());
    registry.addDefaultSink(sinks.back());
  }
  EXPECT_THROW(registry.addDefaultSink(DummySink::create()), std::runtime_error);

  // The registry still creates channels, with the eight sinks it accepted.
  std::shared_ptr<LogChannel> channel;
  ASSERT_NO_THROW(channel = registry.getChannel("controller"));
  EXPECT_EQ(channel->getNumberOfSinks(), LogChannel::kMaxSinks);

  // A sink that is a default sink already is not a new one.
  EXPECT_NO_THROW(registry.addDefaultSink(sinks.front()));
  EXPECT_EQ(registry.getChannel("controller")->getNumberOfSinks(), LogChannel::kMaxSinks);
}
