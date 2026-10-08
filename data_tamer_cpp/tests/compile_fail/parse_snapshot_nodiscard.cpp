// Compile-fail probe (see data_tamer_compile_fail_test in tests/CMakeLists.txt).
// As written the result of ParseSnapshot() is used and the file compiles.
// DT_IGNORE_PARSE_SNAPSHOT drops it, which must then be an error.
#include "data_tamer_parser/data_tamer_parser.hpp"

#include <string>

bool decode(const DataTamerParser::Schema& schema,
            const DataTamerParser::SnapshotView& view)
{
  auto callback = [](const std::string&, const DataTamerParser::VarNumber&) {};
#ifdef DT_IGNORE_PARSE_SNAPSHOT
  DataTamerParser::ParseSnapshot(schema, view, callback);
  return true;
#else
  return DataTamerParser::ParseSnapshot(schema, view, callback);
#endif
}
