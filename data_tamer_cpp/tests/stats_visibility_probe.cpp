// Built as a shared object by tests/CMakeLists.txt; check_no_stats_symbol.cmake
// then asserts that its dynamic symbol table does not export any stats().
#include "data_tamer/channel.hpp"
#include "data_tamer/data_sink.hpp"
#include "data_tamer/sinks/mcap_ring_sink.hpp"

extern "C" void data_tamer_stats_probe(DataTamer::LogChannel* channel,
                                       DataTamer::SinkWorker* worker,
                                       DataTamer::MCAPRingSink* ring)
{
  (void)channel->stats();
  (void)worker->stats();
  (void)ring->stats();
}
