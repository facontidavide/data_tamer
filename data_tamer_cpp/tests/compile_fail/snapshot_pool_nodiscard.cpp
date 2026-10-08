// Compile-fail probe (see data_tamer_compile_fail_test in tests/CMakeLists.txt).
// As written both results are used and the file compiles. DT_IGNORE_TRY_ACQUIRE and
// DT_IGNORE_ADOPT each drop one use, which must then be an error.
#include "data_tamer/details/snapshot_pool.hpp"

#include <memory>

namespace probe
{
DataTamer::SnapshotRef takeAndWrap(const std::shared_ptr<DataTamer::SnapshotPool>& pool)
{
#ifdef DT_IGNORE_TRY_ACQUIRE
  pool->tryAcquire();
  DataTamer::PoolSlot* slot = nullptr;
#else
  DataTamer::PoolSlot* slot = pool->tryAcquire();
#endif
#ifdef DT_IGNORE_ADOPT
  DataTamer::SnapshotPool::adopt(pool, slot);
  return {};
#else
  return DataTamer::SnapshotPool::adopt(pool, slot);
#endif
}
}  // namespace probe
