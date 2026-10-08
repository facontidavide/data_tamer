// ParseSnapshot() returns false for a snapshot that does not match its schema: ignoring
// that result must not compile quietly.
#include "data_tamer_parser/data_tamer_parser.hpp"

#include <string>

bool decode(const DataTamerParser::Schema& schema,
            const DataTamerParser::SnapshotView& view)
{
  auto callback = [](const std::string&, const DataTamerParser::VarNumber&) {};
#ifdef FAIL_CASE
  DataTamerParser::ParseSnapshot(schema, view, callback);
  return true;
#else
  return DataTamerParser::ParseSnapshot(schema, view, callback);
#endif
}
