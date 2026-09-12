#include "data_tamer/data_sink.hpp"
#include "data_tamer/data_tamer.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef __linux__
#include <unistd.h>
#endif

using namespace DataTamer;

namespace
{
constexpr size_t kSnapshots = 10000;
constexpr auto kPeriod = std::chrono::milliseconds(1);

std::chrono::nanoseconds steadyNow()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch());
}

class LatencySink : public DataSinkBase
{
public:
  LatencySink() { latencies_.reserve(kSnapshots); }
  ~LatencySink() override { finish(); }

  void addChannel(const std::string&, const Schema&) override {}

  void finish()
  {
    stopThread();
    processQueuedSnapshots();
  }

  std::vector<int64_t>& latencies() { return latencies_; }

protected:
  bool storeSnapshot(const Snapshot& snapshot) override
  {
    latencies_.push_back((steadyNow() - snapshot.timestamp).count());
    return true;
  }

private:
  std::vector<int64_t> latencies_;
};

void printLatencies(std::vector<int64_t>& latencies)
{
  if(latencies.empty())
  {
    std::printf("delivery samples=0\n");
    return;
  }

  std::sort(latencies.begin(), latencies.end());
  const auto percentile = [&](double value) {
    return latencies[static_cast<size_t>(double(latencies.size() - 1) * value)];
  };
  std::printf("delivery samples=%zu p50=%lld ns p99=%lld ns p99.9=%lld ns max=%lld ns\n",
              latencies.size(), static_cast<long long>(percentile(0.50)),
              static_cast<long long>(percentile(0.99)),
              static_cast<long long>(percentile(0.999)),
              static_cast<long long>(latencies.back()));
}

#ifdef __linux__
bool readCpuTicks(uint64_t& ticks)
{
  std::ifstream input("/proc/self/stat");
  std::string line;
  if(!std::getline(input, line))
  {
    return false;
  }

  const size_t closing_parenthesis = line.rfind(')');
  if(closing_parenthesis == std::string::npos)
  {
    return false;
  }

  std::istringstream fields(line.substr(closing_parenthesis + 1));
  std::string ignored;
  for(int field = 3; field < 14; ++field)
  {
    if(!(fields >> ignored))
    {
      return false;
    }
  }

  uint64_t user_ticks = 0;
  uint64_t system_ticks = 0;
  if(!(fields >> user_ticks >> system_ticks))
  {
    return false;
  }
  ticks = user_ticks + system_ticks;
  return true;
}
#endif

int measureIdle()
{
  auto sink = std::make_shared<LatencySink>();
#ifdef __linux__
  uint64_t before = 0;
  uint64_t after = 0;
  const long ticks_per_second = sysconf(_SC_CLK_TCK);
  if(ticks_per_second <= 0 || !readCpuTicks(before))
  {
    std::fprintf(stderr, "failed to read process CPU ticks\n");
    return 1;
  }

  const auto start = std::chrono::steady_clock::now();
  std::this_thread::sleep_until(start + std::chrono::seconds(10));
  const auto end = std::chrono::steady_clock::now();
  if(!readCpuTicks(after))
  {
    std::fprintf(stderr, "failed to read process CPU ticks\n");
    return 1;
  }
  sink->finish();

  const double elapsed = std::chrono::duration<double>(end - start).count();
  const uint64_t ticks = after - before;
  const double cpu_percent = 100.0 * double(ticks) / double(ticks_per_second) / elapsed;
  std::printf("idle ticks=%llu clock_ticks_per_second=%ld elapsed_s=%.6f "
              "cpu_percent=%.3f\n",
              static_cast<unsigned long long>(ticks), ticks_per_second, elapsed,
              cpu_percent);
#else
  sink->finish();
  std::printf("idle CPU measurement unavailable on this platform\n");
#endif
  return 0;
}

int measureDelivery()
{
  auto channel = LogChannel::create("sink_latency");
  auto sink = std::make_shared<LatencySink>();
  channel->addDataSink(sink);
  uint64_t value = 0;
  channel->registerValue("value", &value);

  size_t successful = 0;
  size_t failed = 0;
  auto next = std::chrono::steady_clock::now();
  for(size_t i = 0; i < kSnapshots; ++i)
  {
    next += kPeriod;
    std::this_thread::sleep_until(next);
    value = i;
    if(channel->takeSnapshot(steadyNow()))
    {
      ++successful;
    }
    else
    {
      ++failed;
    }
  }

  sink->finish();
  std::printf("sink_latency snapshots=%zu rate_hz=1000\n", kSnapshots);
  std::printf("submissions successful=%zu failed=%zu\n", successful, failed);
  printLatencies(sink->latencies());
  return 0;
}
}  // namespace

int main(int argc, char** argv)
{
  if(argc == 1)
  {
    return measureDelivery();
  }
  if(argc == 2 && std::strcmp(argv[1], "--idle") == 0)
  {
    return measureIdle();
  }

  std::fprintf(stderr, "unknown option %s\n", argv[argc > 2 ? 2 : 1]);
  return 1;
}
