// requestDump() and dumpRequested() are called from real-time threads: they cannot throw.
#include "data_tamer/sinks/mcap_ring_sink.hpp"

#include <chrono>
#include <type_traits>

using DataTamer::MCAPRingSink;

// A trait rather than noexcept(sink.requestDump()): that expression also builds the
// default argument, whose std::chrono constructor is not declared noexcept.
static_assert(std::is_nothrow_invocable_r_v<bool, decltype(&MCAPRingSink::requestDump),
                                            MCAPRingSink&, std::chrono::nanoseconds>,
              "requestDump() must be noexcept");
static_assert(std::is_nothrow_invocable_r_v<bool, decltype(&MCAPRingSink::dumpRequested),
                                            const MCAPRingSink&>,
              "dumpRequested() must be noexcept");
