// Compile-fail probe (see data_tamer_compile_fail_test in tests/CMakeLists.txt),
// compiled as for a big endian host. As written it compiles. DT_SWAP_LONG_DOUBLE
// byte-swaps a type of no 1, 2, 4 or 8 byte size, which must then be an error.
#include "data_tamer/contrib/SerializeMe.hpp"

long double swapped(long double value)
{
#ifdef DT_SWAP_LONG_DOUBLE
  return SerializeMe::EndianSwap(value);
#else
  return value;
#endif
}
