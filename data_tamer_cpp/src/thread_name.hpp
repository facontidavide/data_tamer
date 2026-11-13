#pragma once

#if defined(__linux__) || defined(__APPLE__)
#include <pthread.h>
#endif

namespace DataTamer2
{
namespace details
{
/// Names the calling thread, as shown by `top -H`, `ps -L`, perf and debuggers. Linux
/// keeps 15 characters and refuses a longer name (the thread then keeps the process
/// name), so pass a short literal. Does nothing on other platforms.
inline void setCurrentThreadName(const char* name) noexcept
{
#if defined(__linux__)
  pthread_setname_np(pthread_self(), name);
#elif defined(__APPLE__)
  pthread_setname_np(name);
#else
  (void)name;
#endif
}

}  // namespace details
}  // namespace DataTamer2
