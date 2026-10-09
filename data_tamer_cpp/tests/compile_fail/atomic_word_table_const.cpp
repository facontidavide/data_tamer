// Compile-fail probe (see data_tamer_compile_fail_test in tests/CMakeLists.txt).
// A const AtomicWordTable can be read but not written. As written the file reads
// through the const table and writes through a mutable one; DT_WRITE_THROUGH_CONST
// adds a write through the const table.
#include "data_tamer/details/shared_state.hpp"

namespace probe
{
uint32_t readConst(const DataTamer::AtomicWordTable& table)
{
#ifdef DT_WRITE_THROUGH_CONST
  table[0].store(1);
#endif
  return table[0].load();
}

void writeMutable(DataTamer::AtomicWordTable& table)
{
  table[0].store(2);
}
}  // namespace probe
