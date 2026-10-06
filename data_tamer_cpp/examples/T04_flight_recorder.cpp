// Flight recorder: keep the last second of data in RAM and write it to an MCAP
// file only when something goes wrong, plus half a second after the event.
#include "data_tamer/channel.hpp"
#include "data_tamer/data_tamer.hpp"
#include "data_tamer/sinks/mcap_ring_sink.hpp"

#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>

using namespace DataTamer;
using namespace std::chrono_literals;

int main()
{
  MCAPRingOptions options;
  options.filepath = "fault.mcap";  // dumps: fault_1.mcap, fault_2.mcap, ...
  options.window = 1s;
  options.capacity_bytes = 4 * 1024 * 1024;
  auto worker = MCAPRingSink::create(options);
  auto& recorder = worker->as<MCAPRingSink>();
  recorder.setDumpCallback([](const MCAPRingDump& dump) {
    std::cout << (dump.ok ? "wrote " : "failed ") << dump.path << " (" << dump.messages
              << " messages) " << dump.error << std::endl;
  });

  auto channel = LogChannel::create("control");
  double position = 0;
  double velocity = 0;
  channel->registerValue("position", &position);
  channel->registerValue("velocity", &velocity);
  channel->addDataSink(worker);
  channel->prepare();

  // A 1 kHz control loop; a fault at t = 2 s.
  for(int i = 0; i < 3000; i++)
  {
    const double t = i * 0.001;
    position = std::sin(t);
    velocity = std::cos(t);
    (void)channel->tryTakeSnapshot();
    if(i == 2000)
    {
      // Real-time safe: one atomic compare-exchange, no allocation.
      recorder.requestDump(500ms);
    }
    std::this_thread::sleep_for(1ms);
  }

  // Write a dump requested just before shutdown, if any, and wait for the writer.
  worker->stop();
  recorder.flushPendingDump();
  return 0;
}
