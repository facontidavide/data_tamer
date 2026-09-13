#include "data_tamer/data_sink.hpp"
#include "data_tamer/sinks/mcap_sink.hpp"

#ifdef USING_ROS2
#include "data_tamer/sinks/ros2_publisher_sink.hpp"
#endif

#include <gtest/gtest.h>

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
  static_assert(sizeof(SinkWorker) == 2 * sizeof(std::unique_ptr<int>), "SinkWorker grew "
                                                                        "a member "
                                                                        "outside its "
                                                                        "Pimpl");
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
