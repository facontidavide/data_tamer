#pragma once

namespace DataTamer
{
namespace details
{
/// CPU hint for one busy-wait iteration (pause, yield). Also a compiler barrier, so the
/// loop reloads what it polls.
inline void spinPause() noexcept
{
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
  asm volatile("" ::: "memory");
#elif defined(__aarch64__) || (defined(__arm__) && defined(__ARM_ARCH) && __ARM_ARCH >= 7)
  asm volatile("yield" ::: "memory");
#elif defined(__GNUC__)
  asm volatile("" ::: "memory");
#endif
}
}  // namespace details
}  // namespace DataTamer
