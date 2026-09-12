// Latency-distribution harness: a periodic loop calling takeSnapshot() and
// recording every call's duration. Prints p50/p99/p99.9/max, allocation
// count per call after warm-up, and the channel's counters.
//
// usage: rt_latency [--values N] [--sinks K] [--writers W] [--seconds S]
//                   [--rate HZ] [--mcap PATH] [--fifo] [--transactions]
//                   [--vector-writer]
#include "data_tamer/data_sink.hpp"
#include "data_tamer/data_tamer.hpp"
#include "data_tamer/sinks/mcap_sink.hpp"
#include "alloc_counter.hpp"
#include "null_sink.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <pthread.h>
#include <sched.h>
#include <time.h>

using namespace DataTamer;

struct Options
{
  int values = 1000;
  int sinks = 1;
  int writers = 0;
  int seconds = 10;
  int rate_hz = 1000;
  std::string mcap;
  bool fifo = false;
  bool transactions = false;
  bool vector_writer = false;
};

static Options parse(int argc, char** argv)
{
  Options o;
  for(int i = 1; i < argc; i++)
  {
    auto nextArgument = [&]() -> const char* {
      if(++i == argc)
      {
        std::fprintf(stderr, "invalid option %s: missing value\n", argv[i - 1]);
        std::exit(1);
      }
      return argv[i];
    };
    auto nextInteger = [&](int& dst) {
      const char* value = nextArgument();
      const char* end = value + std::strlen(value);
      const auto result = std::from_chars(value, end, dst);
      if(result.ec != std::errc{} || result.ptr != end)
      {
        std::fprintf(stderr, "invalid option %s: expected an integer, got %s\n", argv[i - 1],
                     value);
        std::exit(1);
      }
    };
    if(!std::strcmp(argv[i], "--values")) nextInteger(o.values);
    else if(!std::strcmp(argv[i], "--sinks")) nextInteger(o.sinks);
    else if(!std::strcmp(argv[i], "--writers")) nextInteger(o.writers);
    else if(!std::strcmp(argv[i], "--seconds")) nextInteger(o.seconds);
    else if(!std::strcmp(argv[i], "--rate")) nextInteger(o.rate_hz);
    else if(!std::strcmp(argv[i], "--mcap")) o.mcap = nextArgument();
    else if(!std::strcmp(argv[i], "--fifo")) o.fifo = true;
    else if(!std::strcmp(argv[i], "--transactions")) o.transactions = true;
    else if(!std::strcmp(argv[i], "--vector-writer")) o.vector_writer = true;
    else
    {
      std::fprintf(stderr, "unknown option %s\n", argv[i]);
      std::exit(1);
    }
  }
  if(o.values < 0 || o.sinks < 0 || o.writers < 0 || o.seconds <= 0 || o.rate_hz <= 0)
  {
    std::fprintf(stderr,
                 "invalid option values: counts must be nonnegative; seconds and rate must be positive\n");
    std::exit(1);
  }
  if(o.sinks > 8)
  {
    std::fprintf(stderr, "invalid option --sinks: at most eight sinks are supported\n");
    std::exit(1);
  }
  if(o.values < 2)
  {
    o.values = 2;  // half plain, half LoggedValue: need at least one of each
  }
  return o;
}

static bool trySchedFifo()
{
  sched_param sp{};
  sp.sched_priority = 80;
  return pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0;
}

static void printPercentiles(std::vector<long>& ns)
{
  std::sort(ns.begin(), ns.end());
  auto pct = [&](double p) { return ns[size_t(double(ns.size() - 1) * p)]; };
  std::printf("samples=%zu p50=%ld ns p99=%ld ns p99.9=%ld ns max=%ld ns\n", ns.size(),
              pct(0.50), pct(0.99), pct(0.999), ns.back());
}

int main(int argc, char** argv)
{
  const Options opt = parse(argc, argv);
  std::printf("rt_latency values=%d sinks=%d writers=%d seconds=%d rate=%dHz mcap=%s fifo=%d transactions=%d vector_writer=%d\n",
              opt.values, opt.sinks, opt.writers, opt.seconds, opt.rate_hz,
              opt.mcap.empty() ? "-" : opt.mcap.c_str(), int(opt.fifo),
              int(opt.transactions), int(opt.vector_writer));

  auto channel = LogChannel::create("rt");
  std::vector<std::shared_ptr<DataSinkBase>> sinks;
  for(int i = 0; i < opt.sinks; i++)
  {
    if(!opt.mcap.empty() && i == 0)
    {
      auto mcap = std::make_shared<MCAPSink>(opt.mcap, /*compression*/ true);
      mcap->setMaxTimeBeforeReset(std::chrono::seconds(0));
      sinks.push_back(mcap);
    }
    else
    {
      sinks.push_back(std::make_shared<NullSink>());
    }
    channel->addDataSink(sinks.back());
  }

  // half the values as one registered vector, half as LoggedValues the writers touch
  std::vector<double> plain(size_t(opt.values / 2));
  channel->registerValue("plain", &plain);
  std::vector<std::shared_ptr<LoggedValue<double>>> logged;
  for(int i = 0; i < opt.values / 2; i++)
  {
    logged.push_back(channel->createLoggedValue<double>("lv" + std::to_string(i)));
  }
  std::shared_ptr<LoggedValue<std::vector<double>>> vector_value;
  if(opt.vector_writer)
  {
    vector_value = channel->createLoggedValue<std::vector<double>>("vector_writer");
  }

  std::atomic_bool run{ true };
  std::vector<std::thread> writers;
  for(int w = 0; w < opt.writers; w++)
  {
    writers.emplace_back([&, w] {
      double x = double(w);
      while(run)
      {
        if(opt.transactions)
        {
          auto transaction = channel->scopedWrite();
          for(size_t i = size_t(w); i < logged.size(); i += size_t(opt.writers))
          {
            logged[i]->set(x);
          }
        }
        else
        {
          for(size_t i = size_t(w); i < logged.size(); i += size_t(opt.writers))
          {
            logged[i]->set(x);
          }
        }
        x += 1.0;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
      }
    });
  }
  if(vector_value)
  {
    writers.emplace_back([&] {
      size_t size = 1;
      while(run)
      {
        vector_value->set(std::vector<double>(size, double(size)));
        size = size == 64 ? 1 : size + 1;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
      }
    });
  }

  const bool fifo_ok = opt.fifo && trySchedFifo();
  if(opt.fifo && !fifo_ok)
  {
    std::printf("SCHED_FIFO not available (need CAP_SYS_NICE); running SCHED_OTHER\n");
  }

  // warm-up
  for(int i = 0; i < 10; i++)
  {
    channel->takeSnapshot();
  }

  const long period_ns = 1000000000L / opt.rate_hz;
  const size_t total = size_t(opt.seconds) * size_t(opt.rate_hz);
  std::vector<long> durations;
  durations.reserve(total);
  std::size_t allocations = 0;
  size_t failed = 0;

  const size_t plain_size = plain.size();
  timespec next{};
  clock_gettime(CLOCK_MONOTONIC, &next);
  for(size_t i = 0; i < total; i++)
  {
    next.tv_nsec += period_ns;
    while(next.tv_nsec >= 1000000000L)
    {
      next.tv_nsec -= 1000000000L;
      next.tv_sec += 1;
    }
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, nullptr);

    plain[i % plain_size] = double(i);
    DataTamerTest::AllocCounter::Scope scope;
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = channel->takeSnapshot();
    const auto t1 = std::chrono::steady_clock::now();
    allocations += scope.allocations();
    if(!ok)
    {
      failed++;
    }
    durations.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
  }

  run = false;
  for(auto& t : writers)
  {
    t.join();
  }

  for(const auto& sink : sinks)
    if(auto mcap = std::dynamic_pointer_cast<MCAPSink>(sink))
      mcap->finishQueueAndStop();

  printPercentiles(durations);
  const auto stats = channel->stats();
  std::printf("write_lock_contended=%llu write_lock_wait_max_ns=%llu\n",
              (unsigned long long)stats.write_lock_contended,
              (unsigned long long)stats.write_lock_wait_max_ns);
  uint64_t attachment_drops = 0;
  for(const auto& sink : sinks) attachment_drops += channel->droppedSnapshots(sink);
  std::printf("pool_exhausted=%llu payload_reallocations=%llu dropped_oversize=%llu attachment_drops=%llu\n",
              (unsigned long long)stats.pool_exhausted,
              (unsigned long long)stats.payload_reallocations,
              (unsigned long long)stats.dropped_oversize,
              (unsigned long long)attachment_drops);
  std::printf("allocations per call after warm-up: %.4f\n", double(allocations) / double(total));
  std::printf("allocations after warm-up: %zu\n", allocations);
  std::printf("takeSnapshot returned false: %zu / %zu\n", failed, total);
  std::printf("fifo=%d\n", int(fifo_ok));
  return 0;
}
