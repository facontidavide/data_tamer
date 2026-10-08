#include "data_tamer/channel.hpp"
#include "data_tamer/data_tamer.hpp"
#include "data_tamer/sinks/mcap_sink.hpp"
#include <iostream>
#include <cmath>
#include <thread>

using namespace DataTamer;

void WritingThread(const std::string& channel_name)
{
  std::cout << "Starting thread: " << channel_name << std::endl;

  // Create (or get) a channel using the global registry (singleton)
  auto channel = ChannelsRegistry::Global().getChannel(channel_name);
  long time_diff_nsec = 0;

  auto const vect_size = 100;
  std::vector<double> real64(vect_size);
  std::vector<float> real32(vect_size);
  std::vector<int16_t> int16(vect_size);

  enum Color : uint8_t
  {
    RED = 0,
    GREEN = 1,
    BLUE = 2
  };
  Color color = RED;

  const RegistrationID ids[] = { channel->registerValue("real64", &real64),
                                 channel->registerValue("real32", &real32),
                                 channel->registerValue("int16", &int16),
                                 channel->registerValue("color", &color) };

  int count = 0;
  double t = 0;
  while(t < 10)  // 10 simulated seconds
  {
    auto S = std::sin(t);
    for(size_t i = 0; i < vect_size; i++)
    {
      const auto value = static_cast<double>(i) + S;
      real64[i] = value;
      real32[i] = float(value);
      int16[i] = int16_t(10 * value);
    }
    color = static_cast<Color>(count % 3);

    if(count++ % 1000 == 0)
    {
      printf("[%s] time: %.1f\n", channel_name.c_str(), t);
      std::flush(std::cout);
    }
    auto t1 = std::chrono::system_clock::now();
    if(channel->takeSnapshot() != SnapshotResult::ok)
    {
      std::cout << "pushing failed\n";
    }
    auto t2 = std::chrono::system_clock::now();

    auto diff = std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1);
    time_diff_nsec += diff.count();

    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    t += 0.001;
  }
  std::cout << "average execution time of takeSnapshot(): " << time_diff_nsec / count
            << " nanoseconds" << std::endl;

  // The channel outlives this thread in the registry: stop it reading these locals.
  for(const auto& id : ids)
  {
    channel->unregister(id);
  }
}

int main()
{
  // Sinks added by default reach every channel of the registry.
  auto mcap_sink = MCAPSink::create("test_1M.mcap");
  ChannelsRegistry::Global().addDefaultSink(mcap_sink);

  // Each thread logs 301 values (three vectors of 100 and a color) at 1 kHz for
  // 10 seconds: about 12 million data points in total.
  const int N = 4;
  std::thread writers[N];
  for(int i = 0; i < N; i++)
  {
    writers[i] = std::thread(WritingThread, std::string("channel_") + std::to_string(i));
  }

  for(int i = 0; i < N; i++)
  {
    writers[i].join();
  }
  ChannelsRegistry::Global().stopAll();  // close test_1M.mcap
}
