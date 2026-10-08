// Compile-fail probe (see data_tamer_compile_fail_test in tests/CMakeLists.txt).
// The text renderings of a schema exist for their result. As written all three results
// are used; each DT_IGNORE_* macro drops one use.
#include "data_tamer/types.hpp"

#include <string>

namespace probe
{
std::string render(const DataTamer::Schema& schema)
{
#ifdef DT_IGNORE_TO_STR
  DataTamer::ToStr(schema);
#endif
#ifdef DT_IGNORE_TO_YAML
  DataTamer::ToYaml(schema);
#endif
#ifdef DT_IGNORE_RENDER_SCHEMA
  DataTamer::RenderSchema(schema, DataTamer::SchemaFormat::Text);
#endif
  return DataTamer::ToStr(schema) + DataTamer::ToYaml(schema) +
         DataTamer::RenderSchema(schema, DataTamer::SchemaFormat::Yaml);
}
}  // namespace probe
