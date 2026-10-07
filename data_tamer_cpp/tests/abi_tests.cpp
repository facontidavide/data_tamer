#include "data_tamer/channel.hpp"
#include "data_tamer/data_sink.hpp"
#include "data_tamer/data_tamer.hpp"
#include "data_tamer/sinks/mcap_sink.hpp"

#ifdef USING_ROS2
#include "data_tamer/sinks/ros2_publisher_sink.hpp"
#endif

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <memory>

using namespace DataTamer;

TEST(ABI, SinksAreOnlyOnePointerLargerThanTheInterface)
{
  static_assert(sizeof(MCAPSink) == sizeof(DataSink) + sizeof(std::unique_ptr<int>), "MCA"
                                                                                     "PSi"
                                                                                     "nk "
                                                                                     "gre"
                                                                                     "w "
                                                                                     "a "
                                                                                     "mem"
                                                                                     "ber"
                                                                                     " ou"
                                                                                     "tsi"
                                                                                     "de "
                                                                                     "its"
                                                                                     " Pi"
                                                                                     "mp"
                                                                                     "l");
#ifdef USING_ROS2
  static_assert(
      sizeof(ROS2PublisherSink) == sizeof(DataSink) + sizeof(std::unique_ptr<int>), "ROS2"
                                                                                    "Publ"
                                                                                    "ishe"
                                                                                    "rSin"
                                                                                    "k "
                                                                                    "grew"
                                                                                    " a "
                                                                                    "memb"
                                                                                    "er "
                                                                                    "outs"
                                                                                    "ide "
                                                                                    "its "
                                                                                    "Pimp"
                                                                                    "l");
#endif
  // The sink and the per-channel queues live in the Pimpl: one pointer.
  static_assert(sizeof(SinkWorker) == sizeof(std::unique_ptr<int>), "SinkWorker grew a "
                                                                    "member outside its "
                                                                    "Pimpl");
  // The types registry lives in the Pimpl too.
  static_assert(sizeof(LogChannel) == sizeof(std::enable_shared_from_this<LogChannel>) +
                                          sizeof(std::unique_ptr<int>),
                "LogChannel grew a member outside its Pimpl");
  static_assert(sizeof(SnapshotRef) == sizeof(std::shared_ptr<int>) + sizeof(void*), "Sna"
                                                                                     "psh"
                                                                                     "otR"
                                                                                     "ef "
                                                                                     "lay"
                                                                                     "out"
                                                                                     " is"
                                                                                     " pa"
                                                                                     "rt "
                                                                                     "of "
                                                                                     "the"
                                                                                     " AB"
                                                                                     "I");
}

// DataSink's vtable and layout are frozen for 2.x (see data_sink.hpp): no data
// members. ChannelDefaults crosses the boundary by value: a layout change needs
// a SONAME bump.
TEST(ABI, DataSinkHasNoDataAndChannelDefaultsIsPinned)
{
  static_assert(sizeof(DataSink) == sizeof(void*), "DataSink gained a data member");
  static_assert(sizeof(ChannelDefaults) ==
                    2 * sizeof(size_t) + 2 * sizeof(std::chrono::nanoseconds),
                "ChannelDefaults layout is part of the ABI");
}
