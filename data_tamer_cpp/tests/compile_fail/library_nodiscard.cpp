// Compile-fail probe (see data_tamer_compile_fail_test in tests/CMakeLists.txt).
// The results of these library functions are there to be used. As written every result
// is cast to void and the file compiles; each DT_IGNORE_* macro drops one cast, which
// must then be an error.
#include "data_tamer/channel.hpp"
#include "data_tamer/details/snapshot_pool.hpp"
#include "data_tamer/sinks/mcap_ring_sink.hpp"
#include "data_tamer/types.hpp"

#include <memory>

namespace probe
{
void useResults(const std::shared_ptr<DataTamer::SnapshotPool>& pool,
                DataTamer::PoolSlot* slot, const DataTamer::Schema& schema,
                DataTamer::MCAPRingSink& ring_sink, DataTamer::LogChannel& channel,
                const DataTamer::RegistrationID& id)
{
  using namespace DataTamer;
#ifdef DT_IGNORE_POOL_TRY_ACQUIRE
  pool->tryAcquire();
#else
  (void)pool->tryAcquire();
#endif
#ifdef DT_IGNORE_POOL_ADOPT
  SnapshotPool::adopt(pool, slot);
#else
  (void)SnapshotPool::adopt(pool, slot);
#endif
#ifdef DT_IGNORE_TO_STR
  ToStr(schema);
#else
  (void)ToStr(schema);
#endif
#ifdef DT_IGNORE_TO_YAML
  ToYaml(schema);
#else
  (void)ToYaml(schema);
#endif
#ifdef DT_IGNORE_RENDER_SCHEMA
  RenderSchema(schema, SchemaFormat::Text);
#else
  (void)RenderSchema(schema, SchemaFormat::Text);
#endif
#ifdef DT_IGNORE_REQUEST_DUMP
  ring_sink.requestDump();
#else
  (void)ring_sink.requestDump();
#endif
#ifdef DT_IGNORE_TRY_SET_ENABLED
  channel.trySetEnabled(id, true);
#else
  (void)channel.trySetEnabled(id, true);
#endif
#ifdef DT_IGNORE_CREATE
  LogChannel::create("probe");
#else
  (void)LogChannel::create("probe");
#endif
#ifdef DT_IGNORE_NUMBER_OF_SINKS
  channel.getNumberOfSinks();
#else
  (void)channel.getNumberOfSinks();
#endif
}
}  // namespace probe
