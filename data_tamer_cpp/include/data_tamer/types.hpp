#pragma once

#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>
#include <string_view>
#include <map>
#include <vector>
#include <variant>

namespace DataTamer
{

constexpr int SCHEMA_VERSION = 5;
/// Version of the YAML rendering of a schema (see ToYaml()).
constexpr int SCHEMA_YAML_VERSION = 6;

// clang-format off
enum class BasicType: uint8_t
{
  BOOL,
  CHAR,
  INT8,
  UINT8,

  INT16,
  UINT16,

  INT32,
  UINT32,

  INT64,
  UINT64,

  FLOAT32,
  FLOAT64,
  OTHER
};

constexpr size_t TypesCount = 13;


using VarNumber = std::variant<
    bool, char,
    int8_t, uint8_t,
    int16_t, uint16_t,
    int32_t, uint32_t,
    int64_t, uint64_t,
    float, double >;
// clang-format on

/// Reads one scalar of `type` from `data` (the reverse of ValuePtr::serialize); NaN
/// for BasicType::OTHER.
[[nodiscard]] VarNumber DeserializeAsVarType(const BasicType& type, const void* data);

/// Serialized size of a scalar type in bytes; 0 for BasicType::OTHER.
[[nodiscard]] size_t SizeOf(const BasicType& type);

/// Name of the type in the schema, e.g. "float64".
[[nodiscard]] const std::string& ToStr(const BasicType& type);

/// Inverse of ToStr(); BasicType::OTHER for an unknown name.
[[nodiscard]] BasicType FromStr(const std::string& str);

template <typename T>
inline constexpr BasicType GetBasicType()
{
  if constexpr(std::is_enum_v<T>)
  {
    return GetBasicType<std::underlying_type_t<T>>();
  }

  // clang-format off
  if constexpr (std::is_same_v<T, bool>) return BasicType::BOOL;
  if constexpr (std::is_same_v<T, char>) return BasicType::CHAR;
  if constexpr (std::is_same_v<T, std::int8_t>) return BasicType::INT8;
  if constexpr (std::is_same_v<T, std::uint8_t>) return BasicType::UINT8;

  if constexpr (std::is_same_v<T, std::int16_t>) return BasicType::INT16;
  if constexpr (std::is_same_v<T, std::uint16_t>) return BasicType::UINT16;

  if constexpr (std::is_same_v<T, std::int32_t>) return BasicType::INT32;
  if constexpr (std::is_same_v<T, std::uint32_t>) return BasicType::UINT32;

  if constexpr (std::is_same_v<T, std::uint64_t>) return BasicType::UINT64;
  if constexpr (std::is_same_v<T, std::int64_t>) return BasicType::INT64;

  if constexpr (std::is_same_v<T, float>) return BasicType::FLOAT32;
  if constexpr (std::is_same_v<T, double>) return BasicType::FLOAT64;
  // clang-format on
  return BasicType::OTHER;
}

template <typename T>
inline constexpr bool IsNumericType()
{
  return std::is_arithmetic_v<T> || std::is_same_v<T, bool> || std::is_same_v<T, char> ||
         std::is_enum_v<T>;
}

class LogChannel;
class ChannelSharedState;
template <typename T>
class LoggedValue;

/**
 * @brief Handle to one registration, returned by LogChannel::registerValue and friends.
 * After unregister() and a new registration of the same name the old handle is stale
 * and is rejected. A default-constructed handle is never valid. A handle from another
 * channel is not detected.
 */
class RegistrationID
{
public:
  RegistrationID() = default;

  bool operator==(const RegistrationID& other) const
  {
    return index_ == other.index_ && generation_ == other.generation_;
  }
  bool operator!=(const RegistrationID& other) const { return !(*this == other); }

private:
  friend class LogChannel;
  friend class ChannelSharedState;
  template <typename T>
  friend class LoggedValue;
  friend struct std::hash<RegistrationID>;

  RegistrationID(uint32_t index, uint32_t generation)
    : index_(index), generation_(generation)
  {}

  uint32_t index_ = 0;
  uint32_t generation_ = 0;  // 0: never valid; slots start at 1
};

//---------------------------------------------------------
struct TypeField
{
  std::string field_name;
  BasicType type = BasicType::OTHER;
  std::string type_name;
  bool is_vector = 0;
  uint32_t array_size = 0;

  bool operator==(const TypeField& other) const;
  bool operator!=(const TypeField& other) const;

  friend std::ostream& operator<<(std::ostream& os, const TypeField& field);
};

using FieldsVector = std::vector<TypeField>;
class CustomSerializer;

struct CustomSchema
{
  std::string encoding;
  std::string schema;
};

/**
 * @brief The schema of a channel: a flat list of fields plus the custom types they
 * use. `hash` must equal ComputeSchemaHash().
 */
struct Schema
{
  uint64_t hash = 0;
  FieldsVector fields;
  std::string channel_name;

  std::map<std::string, FieldsVector> custom_types;  // sorted: deterministic schema text
  std::map<std::string, CustomSchema> custom_schemas;

  friend std::ostream& operator<<(std::ostream& os, const Schema& schema);
};

std::string ToStr(const Schema& schema);

/**
 * @brief The schema as YAML, schema version 6 (docs/wire_format.md, section 2.1):
 * shorter when field names are "/"-separated paths. Schema::hash is the same as for
 * ToStr(), because it is always computed over that text.
 */
std::string ToYaml(const Schema& schema);

/// Rendering of a schema text. Both carry the same Schema::hash; parsers detect which.
enum class SchemaFormat
{
  /// ToStr(): the line format, schema version 5. Every reader understands it.
  Text,
  /// ToYaml(): schema version 6. Older readers (e.g. older PlotJuggler) cannot read it.
  Yaml,
};

/// ToStr(schema) or ToYaml(schema), depending on `format`.
std::string RenderSchema(const Schema& schema, SchemaFormat format);

/**
 * @brief FNV-1a 64 over a schema text without its "### hash:" line (docs/wire_format.md,
 * section 5). Any decoder can recompute it.
 */
[[nodiscard]] uint64_t SchemaTextHash(std::string_view schema_text);

/// SchemaTextHash() of ToStr(schema); the value Schema::hash must hold.
[[nodiscard]] uint64_t ComputeSchemaHash(const Schema& schema);

}  // namespace DataTamer

template <>
struct std::hash<DataTamer::RegistrationID>
{
  std::size_t operator()(const DataTamer::RegistrationID& id) const
  {
    // Combines index and generation as in http://stackoverflow.com/a/1646913/126995
    std::size_t res = 17;
    res = res * 31 + hash<uint32_t>()(id.index_);
    res = res * 31 + hash<uint32_t>()(id.generation_);
    return res;
  }
};
