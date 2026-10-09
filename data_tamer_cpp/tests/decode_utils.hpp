#pragma once

#include "data_tamer/channel.hpp"
#include "data_tamer_parser/data_tamer_parser.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <string>
#include <variant>

namespace DataTamerTest
{

/// The standalone parser's view of `snapshot`. It points into the snapshot, which must
/// outlive it.
inline DataTamerParser::SnapshotView viewOf(const DataTamer::Snapshot& snapshot)
{
  return { snapshot.schema_hash,
           uint64_t(snapshot.timestamp.count()),
           { snapshot.active_mask.data(), snapshot.active_mask.size() },
           { snapshot.payload.data(), snapshot.payload.size() } };
}

/// What the standalone parser reads from `snapshot` with the schema `channel` wrote:
/// every number by the name of its field. The payload must end with the last field.
inline std::map<std::string, double> decode(const DataTamer::LogChannel& channel,
                                            const DataTamer::Snapshot& snapshot)
{
  const auto schema =
      DataTamerParser::BuildSchemaFromText(DataTamer::ToStr(channel.getSchema()));
  std::map<std::string, double> values;
  const bool complete = DataTamerParser::ParseSnapshot(
      schema, viewOf(snapshot),
      [&](const std::string& name, const DataTamerParser::VarNumber& number) {
        values[name] = std::visit([](const auto& x) { return double(x); }, number);
      });
  EXPECT_TRUE(complete) << "bytes are left after the last field";
  return values;
}

}  // namespace DataTamerTest
