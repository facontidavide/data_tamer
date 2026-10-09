// The standalone parser is copied into other projects. Compiled as strict C++17 with
// the warnings of this library as errors (see CMakeLists.txt), and with its templates
// instantiated, it must not produce a single warning.
#include "data_tamer_parser/data_tamer_parser.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace
{
// Same field names as the data_tamer_msgs types, without depending on ROS.
struct SchemaMsg
{
  uint64_t hash;
  std::string channel_name;
  std::string schema_text;
};
struct SnapshotMsg
{
  uint64_t timestamp_nsec;
  uint64_t schema_hash;
  std::vector<uint8_t> active_mask;
  std::vector<uint8_t> payload;
};
struct BatchMsg
{
  std::vector<SchemaMsg> schemas;
  std::vector<SnapshotMsg> snapshots;
};

[[maybe_unused]] size_t useEverything(const std::string& text, const BatchMsg& batch)
{
  using namespace DataTamerParser;
  size_t total = 0;

  const Schema schema = BuildSchemaFromText(text, true);
  total += ToText(schema).size() + SchemaTextHash(text) + schema.fields.size();

  const std::vector<uint8_t> body(16);
  const SnapshotView view = SplitMcapMessage({ body.data(), body.size() });
  total += GetBit(view.active_mask, 0) ? 1 : 0;
  auto count = [&](const std::string& name, const VarNumber& value) {
    total += name.size() + value.index();
  };
  total += ParseSnapshot(schema, view, count) ? 1 : 0;

  SchemaRegistry registry;
  total += registry.add(schema.hash, text).fields.size();
  registry.addSchemas(batch);
  total += registry.find(schema.hash) != nullptr ? 1 : 0;
  total += ForEachSnapshotInBatch(registry, batch,
                                  [&](const Schema& s, const SnapshotView& v) {
                                    total += s.fields.size() + v.payload.size;
                                  });
  total += ToSnapshotView(batch.snapshots.front()).payload.size;
  return total;
}
}  // namespace
