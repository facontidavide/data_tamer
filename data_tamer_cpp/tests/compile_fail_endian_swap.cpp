// Compiled as for a big endian host (see CMakeLists.txt). A type with no 1, 2, 4 or
// 8 byte representation cannot be byte-swapped and must be rejected when compiling.
#include "data_tamer/contrib/SerializeMe.hpp"

long double swapped(long double value)
{
#ifdef FAIL_CASE
  return SerializeMe::EndianSwap(value);
#else
  return value;
#endif
}
