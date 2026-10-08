#pragma once

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace DataTamerTest
{

/// Death-test child: exits 2 if `body` still runs after `limit` (a hang), else 3 after
/// a failed assertion (printed to stderr, which the parent shows) or 0.
template <typename Body>
[[noreturn]] void runWithWatchdog(std::chrono::milliseconds limit, Body& body)
{
  std::thread([limit] {
    std::this_thread::sleep_for(limit);
    std::fputs("watchdog: still running after the time limit (deadlock?)\n", stderr);
    std::_Exit(2);
  }).detach();
  body();
  const auto* result = ::testing::UnitTest::GetInstance()->current_test_info()->result();
  for(int i = 0; i < result->total_part_count(); ++i)
  {
    const auto& part = result->GetTestPartResult(i);
    if(part.failed())
    {
      std::fprintf(stderr, "%s:%d: %s\n", part.file_name() ? part.file_name() : "?",
                   part.line_number(), part.message());
    }
  }
  std::_Exit(result->Failed() ? 3 : 0);
}

/// Runs `body` in a child process and expects it to finish without a failed assertion
/// within `limit`: a deadlock then fails this test instead of hanging the whole binary.
/// Threadsafe style: the child re-executes the binary, so the parent's threads don't
/// matter. Everything `body` needs must be created inside it.
template <typename Body>
void expectFinishes(Body body, std::chrono::milliseconds limit = std::chrono::seconds(10))
{
#ifdef GTEST_FLAG_SET
  GTEST_FLAG_SET(death_test_style, "threadsafe");
#else
  ::testing::GTEST_FLAG(death_test_style) = "threadsafe";
#endif
  EXPECT_EXIT(runWithWatchdog(limit, body), ::testing::ExitedWithCode(0), "");
}

}  // namespace DataTamerTest
