// System calls made by the real-time snapshot path.
#include "data_tamer/channel.hpp"
#include "data_tamer/sinks/dummy_sink.hpp"
#include "observed_thread.hpp"
#include "test_sinks.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define DATA_TAMER_TEST_SANITIZED 1  // the runtimes make system calls of their own
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define DATA_TAMER_TEST_SANITIZED 1
#endif
#endif

#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__)) &&               \
    !defined(DATA_TAMER_TEST_SANITIZED)
#define DATA_TAMER_TEST_SECCOMP 1
#include <csignal>
#include <cstddef>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#endif

using namespace DataTamer;

#if defined(DATA_TAMER_TEST_SECCOMP)
namespace
{
std::atomic<int> g_futex_calls{ 0 };

// SIGSYS from the seccomp trap: counts the futex call and makes it return 0, as a
// wake that woke nobody does.
void countFutexCall(int, siginfo_t*, void* context)
{
  g_futex_calls.fetch_add(1, std::memory_order_relaxed);
  auto* uc = static_cast<ucontext_t*>(context);
#if defined(__x86_64__)
  uc->uc_mcontext.gregs[REG_RAX] = 0;
#else
  uc->uc_mcontext.regs[0] = 0;
#endif
}

// Traps futex calls of the calling thread only (no SECCOMP_FILTER_FLAG_TSYNC).
bool trapFutexOnThisThread()
{
#if defined(__x86_64__)
  constexpr uint32_t kArch = AUDIT_ARCH_X86_64;
#else
  constexpr uint32_t kArch = AUDIT_ARCH_AARCH64;
#endif
  sock_filter filter[] = {
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch)),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, kArch, 1, 0),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_futex, 0, 1),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
  };
  sock_fprog program{ static_cast<unsigned short>(sizeof(filter) / sizeof(filter[0])),
                      filter };
  return prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0 &&
         prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0;
}
}  // namespace
#endif

// Threads blocked in std::atomic::wait() elsewhere in the process (a std::latch,
// another library) must not turn tryTakeSnapshot() into a system call: with no
// controller waiting for the snapshot and no stop() waiting for the push, the
// snapshot path makes no futex call, whether the worker runs or is stopped.
TEST(RealTimeSyscalls, TryTakeSnapshotMakesNoFutexCallWhileOtherThreadsWait)
{
#if defined(DATA_TAMER_TEST_SECCOMP)
  constexpr int kSnapshots = 1000;
  auto channel = LogChannel::create("rt_syscalls");
  double value = 1;
  channel->registerValue("value", &value);
  channel->setPoolCapacity(2 * kSnapshots);
  auto sink = DataTamerTest::manual<DummySink>();  // no worker thread to wake
  channel->addDataSink(sink);
  channel->startLogging();

  // 16 consecutive ints reach every bucket of libstdc++'s waiter table.
  alignas(64) std::array<std::atomic<int>, 16> unrelated{};
  std::vector<std::unique_ptr<DataTamerTest::ObservedThread>> waiters;
  struct Release  // wakes the waiters before they are joined, on every path
  {
    ~Release()
    {
      for(auto& word : words)
      {
        word = 1;
        word.notify_all();
      }
    }
    std::array<std::atomic<int>, 16>& words;
  } release{ unrelated };
  for(auto& word : unrelated)
  {
    waiters.push_back(
        std::make_unique<DataTamerTest::ObservedThread>([&word] { word.wait(0); }));
  }
  for(auto& waiter : waiters)
  {
    ASSERT_TRUE(waiter->sleeps());
  }

  struct sigaction action = {};
  action.sa_sigaction = countFutexCall;
  action.sa_flags = SA_SIGINFO;
  struct Restore
  {
    ~Restore() { sigaction(SIGSYS, &previous, nullptr); }
    struct sigaction previous = {};
  } restore;
  sigaction(SIGSYS, &action, &restore.previous);
  std::atomic<int> stage{ 0 };  // 1: no filter, 2: running measured, 3: stopped measured
  std::atomic<bool> worker_stopped{ false };
  int running_calls = -1, stopped_calls = -1;
  // The filter stays on the thread: once measured, it parks without another futex.
  std::thread([&] {
    if(!trapFutexOnThisThread())
    {
      stage = 1;
      return;
    }
    g_futex_calls = 0;
    for(int i = 0; i < kSnapshots; ++i)
    {
      (void)channel->tryTakeSnapshot();
    }
    running_calls = g_futex_calls.exchange(0);
    stage = 2;
    while(!worker_stopped)
    {
    }
    for(int i = 0; i < kSnapshots; ++i)
    {
      (void)channel->tryTakeSnapshot();  // refused by the stopped worker
    }
    stopped_calls = g_futex_calls.exchange(0);
    stage = 3;
    sigset_t all_but_sigsys;
    sigfillset(&all_but_sigsys);
    sigdelset(&all_but_sigsys, SIGSYS);
    pthread_sigmask(SIG_SETMASK, &all_but_sigsys, nullptr);
    while(true)
    {
      pause();
    }
  }).detach();
  while(stage == 0)
  {
    std::this_thread::yield();
  }
  if(stage == 1)
  {
    GTEST_SKIP() << "seccomp filters are not available";
  }
  sink.worker->stop();
  worker_stopped = true;
  while(stage != 3)
  {
    std::this_thread::yield();
  }
  EXPECT_EQ(running_calls, 0) << "futex calls in " << kSnapshots << " snapshots";
  EXPECT_EQ(stopped_calls, 0) << "futex calls in " << kSnapshots
                              << " snapshots refused by a stopped worker";
#else
  GTEST_SKIP() << "counts system calls with seccomp: Linux x86_64 or aarch64, no "
                  "sanitizer";
#endif
}
