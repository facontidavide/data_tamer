#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace DataTamerParser
{

constexpr int SCHEMA_VERSION = 5;
/// Version of the YAML rendering of a schema (docs/wire_format.md, section 2.1).
constexpr int SCHEMA_YAML_VERSION = 6;

enum class BasicType : uint8_t
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

using VarNumber = std::variant<bool, char, int8_t, uint8_t, int16_t, uint16_t, int32_t,
                               uint32_t, int64_t, uint64_t, float, double>;

/// Non-owning view of bytes: the owner must keep them alive while the view is in use.
struct BufferSpan
{
  const uint8_t* data = nullptr;
  size_t size = 0;

  /// Drops the first `n` bytes; throws std::runtime_error if there are fewer.
  void trimFront(size_t n)
  {
    if(n > size)
    {
      throw std::runtime_error("DataTamerParser: payload truncated");
    }
    data += n;
    size -= n;
  }
};

VarNumber DeserializeToVarNumber(BasicType type, BufferSpan& buffer);

//---------------------------------------------------------
/// A schema field. `is_vector` covers `T[]` (array_size 0, a uint32 count precedes the
/// elements) and `T[N]` (array_size N, no count). `type` is OTHER for a custom type.
struct TypeField
{
  std::string field_name;
  BasicType type = BasicType::OTHER;
  std::string type_name;
  bool is_vector = 0;
  uint32_t array_size = 0;

  bool operator==(const TypeField& other) const;
  bool operator!=(const TypeField& other) const;
};

using FieldsVector = std::vector<TypeField>;

/// A custom type whose layout is described in a foreign schema language.
struct CustomSchema
{
  std::string encoding;
  std::string schema;
};

/**
 * @brief A parsed schema (docs/wire_format.md, section 2). `fields[i]` is bit i of the
 * active mask.
 */
struct Schema
{
  uint64_t hash = 0;
  FieldsVector fields;
  std::string channel_name;

  std::map<std::string, FieldsVector> custom_types;
  /// Opaque custom types; filled only from YAML schemas (version 6).
  std::map<std::string, CustomSchema> custom_schemas;
};

struct SnapshotView
{
  /// Hash of the schema that decodes this snapshot.
  uint64_t schema_hash;

  /// Timestamp in nanoseconds, unused by the parser.
  uint64_t timestamp;

  /// One bit per top-level field, least significant bit first (docs/wire_format.md,
  /// section 3.1).
  BufferSpan active_mask;

  /// Values of the active fields, in schema order (docs/wire_format.md, section 3.2).
  BufferSpan payload;
};

bool GetBit(BufferSpan mask, size_t index);

constexpr auto NullCustomCallback = [](const std::string&, const BufferSpan,
                                       const std::string&) {};

/// Decodes `snapshot` with `schema`, calling
///   callback_number(const std::string& name, const VarNumber& value)
/// for each value of each active field, in schema order. Names join nested fields with
/// '/' and index container elements: "pose/position/x", "points[1]/z".
/// Returns false if the hash is not the schema's (nothing is decoded) or if bytes are
/// left after the last field (the callback has already run). Throws std::runtime_error
/// on malformed data. `callback_custom` is unused.
template <typename NumberCallback, typename CustomCallback = decltype(NullCustomCallback)>
bool ParseSnapshot(const Schema& schema, SnapshotView snapshot,
                   const NumberCallback& callback_number,
                   const CustomCallback& callback_custom = NullCustomCallback);

//---------------------------------------------------------
// Helpers for the data_tamer_msgs messages of ROS2PublisherSink. They are templates on
// the message type, so this header needs no ROS: any type with the same fields works.

/**
 * @brief Schemas by hash, filled from the `<prefix>/schemas` topic and/or from the
 * schemas embedded in batches.
 */
class SchemaRegistry
{
public:
  /// Parses and stores `schema_text` under `hash`, unless `hash` is already known.
  /// Throws std::runtime_error if the text is malformed or declares another hash.
  const Schema& add(uint64_t hash, const std::string& schema_text);

  /// Adds every entry of `msg.schemas` (each with `hash` and `schema_text`):
  /// a data_tamer_msgs Schemas or SnapshotBatch message.
  template <typename SchemasMsgT>
  void addSchemas(const SchemasMsgT& msg);

  /// nullptr if the hash is unknown.
  [[nodiscard]] const Schema* find(uint64_t hash) const;

  [[nodiscard]] size_t size() const { return schemas_.size(); }

private:
  std::unordered_map<uint64_t, Schema> schemas_;
};

/// View on a data_tamer_msgs Snapshot (`timestamp_nsec`, `schema_hash`,
/// `active_mask`, `payload`). The message must outlive the view.
template <typename SnapshotMsgT>
SnapshotView ToSnapshotView(const SnapshotMsgT& msg);

/**
 * @brief Calls `callback(const Schema&, const SnapshotView&)` for each snapshot of a
 * data_tamer_msgs SnapshotBatch, in order.
 * The schemas embedded in the batch are added to `registry` first; a malformed one
 * throws (see SchemaRegistry::add) before any snapshot is visited. Snapshots whose
 * schema is not in the registry are skipped.
 *
 * @return the number of snapshots visited.
 */
template <typename BatchMsgT, typename SnapshotCallback>
size_t ForEachSnapshotInBatch(SchemaRegistry& registry, const BatchMsgT& batch,
                              const SnapshotCallback& callback);

//---------------------------------------------------------
//---------------------------------------------------------
//---------------------------------------------------------

/// Reads a T from the front of `buffer` and advances it; throws std::runtime_error if
/// it is too short. Copies the bytes as they are, so the host must be little endian.
template <typename T>
inline T Deserialize(BufferSpan& buffer)
{
  T var;
  const auto N = sizeof(T);
  if(N > buffer.size)
  {
    throw std::runtime_error("DataTamerParser: payload truncated");
  }
  if constexpr(std::is_same_v<T, bool>)
  {
    var = buffer.data[0] != 0;  // another byte value is not a valid bool
  }
  else
  {
    std::memcpy(&var, buffer.data, N);
  }
  buffer.data += N;
  buffer.size -= N;
  return var;
}

inline VarNumber DeserializeToVarNumber(BasicType type, BufferSpan& buffer)
{
  switch(type)
  {
    case BasicType::BOOL:
      return Deserialize<bool>(buffer);
    case BasicType::CHAR:
      return Deserialize<char>(buffer);

    case BasicType::INT8:
      return Deserialize<int8_t>(buffer);
    case BasicType::UINT8:
      return Deserialize<uint8_t>(buffer);

    case BasicType::INT16:
      return Deserialize<int16_t>(buffer);
    case BasicType::UINT16:
      return Deserialize<uint16_t>(buffer);

    case BasicType::INT32:
      return Deserialize<int32_t>(buffer);
    case BasicType::UINT32:
      return Deserialize<uint32_t>(buffer);

    case BasicType::INT64:
      return Deserialize<int64_t>(buffer);
    case BasicType::UINT64:
      return Deserialize<uint64_t>(buffer);

    case BasicType::FLOAT32:
      return Deserialize<float>(buffer);
    case BasicType::FLOAT64:
      return Deserialize<double>(buffer);

    case BasicType::OTHER:
      return double(std::numeric_limits<double>::quiet_NaN());
  }
  return {};
}

inline bool GetBit(BufferSpan mask, size_t index)
{
  if((index >> 3) >= mask.size)
  {
    throw std::runtime_error("DataTamerParser: active mask shorter than the schema");
  }
  const uint8_t& byte = mask.data[index >> 3];
  return 0 != (byte & uint8_t(1 << (index % 8)));
}

/// Folds `field` into a running schema hash with the version 4 recipe. It is std::hash
/// based, so it only reproduces the hash on the writer's platform.
[[nodiscard]] inline uint64_t AddFieldToHash(const TypeField& field, uint64_t hash)
{
  // https://stackoverflow.com/questions/2590677/how-do-i-combine-hash-values-in-c0x
  const std::hash<std::string> str_hasher;
  const std::hash<uint8_t> type_hasher;
  const std::hash<bool> bool_hasher;
  const std::hash<uint32_t> uint_hasher;

  auto combine = [&hash](const auto& hasher, const auto& val) {
    hash ^= hasher(val) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
  };

  combine(str_hasher, field.field_name);
  combine(type_hasher, static_cast<uint8_t>(field.type));
  if(field.type == BasicType::OTHER)
  {
    combine(str_hasher, field.type_name);
  }
  combine(bool_hasher, field.is_vector);
  combine(uint_hasher, field.array_size);
  return hash;
}

/// Hash recipe of schema version 5: FNV-1a 64 of the schema text without its
/// "### hash:" line (docs/wire_format.md, section 5). Platform independent.
[[nodiscard]] inline uint64_t SchemaTextHash(const std::string& text)
{
  uint64_t hash = 0xcbf29ce484222325ULL;
  auto feed = [&hash](const char* begin, const char* end) {
    for(; begin != end; ++begin)
    {
      hash ^= static_cast<uint8_t>(*begin);
      hash *= 0x100000001b3ULL;
    }
  };
  const auto hash_line = text.find("### hash:");
  if(hash_line == std::string::npos)
  {
    feed(text.data(), text.data() + text.size());
    return hash;
  }
  const auto line_end = text.find('\n', hash_line);
  feed(text.data(), text.data() + hash_line);
  if(line_end != std::string::npos)
  {
    feed(text.data() + line_end + 1, text.data() + text.size());
  }
  return hash;
}

inline bool TypeField::operator==(const TypeField& other) const
{
  return is_vector == other.is_vector && type == other.type &&
         array_size == other.array_size && field_name == other.field_name &&
         type_name == other.type_name;
}

inline bool TypeField::operator!=(const TypeField& other) const
{
  return !(*this == other);
}

/// Renders a schema in the version 5 line format, byte for byte as the
/// DataTamer writer does: SchemaTextHash() of the result is the schema hash.
inline std::string ToText(const Schema& schema)
{
  auto fieldLine = [](const TypeField& field) {
    std::string line = field.type_name;
    if(field.is_vector)
    {
      line += field.array_size == 0 ? "[]" : "[" + std::to_string(field.array_size) + "]";
    }
    return line + " " + field.field_name + "\n";
  };
  const std::string separator(59, '=');
  std::string out = "### version: " + std::to_string(SCHEMA_VERSION) +
                    "\n### hash: " + std::to_string(schema.hash) +
                    "\n### channel_name: " + schema.channel_name + "\n\n";
  for(const auto& field : schema.fields)
  {
    out += fieldLine(field);
  }
  for(const auto& [type_name, fields] : schema.custom_types)
  {
    out += separator + "\nMSG: " + type_name + "\n";
    for(const auto& field : fields)
    {
      out += fieldLine(field);
    }
  }
  for(const auto& [type_name, custom] : schema.custom_schemas)
  {
    out += separator + "\nMSG: " + type_name + "\nENCODING: " + custom.encoding + "\n" +
           custom.schema + "\n";
  }
  return out;
}

namespace detail
{
/// Deeper YAML nesting is rejected (the writer nests at most 16 levels).
constexpr size_t kMaxYamlDepth = 64;

struct YamlNode
{
  std::string key;
  std::optional<std::string> scalar;  // empty for a mapping
  std::vector<YamlNode> children;
};

[[noreturn]] inline void YamlError(const std::string& what, const std::string& line)
{
  throw std::runtime_error("DataTamerParser: YAML schema: " + what + " in: " + line);
}

inline void AppendUtf8(std::string& out, uint32_t cp)
{
  if(cp < 0x80)
  {
    out += char(cp);
  }
  else if(cp < 0x800)
  {
    out += char(0xC0 | (cp >> 6));
    out += char(0x80 | (cp & 0x3F));
  }
  else if(cp < 0x10000)
  {
    out += char(0xE0 | (cp >> 12));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  }
  else
  {
    out += char(0xF0 | (cp >> 18));
    out += char(0x80 | ((cp >> 12) & 0x3F));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  }
}

/// Decodes the double-quoted scalar at s[pos] == '"'; returns the index past its end.
inline size_t ReadQuoted(const std::string& s, size_t pos, std::string& out,
                         const std::string& line)
{
  out.clear();
  size_t i = pos + 1;
  while(i < s.size())
  {
    const char c = s[i];
    if(c == '"')
    {
      return i + 1;
    }
    if(c != '\\')
    {
      out += c;
      i++;
      continue;
    }
    if(i + 1 >= s.size())
    {
      break;
    }
    const char e = s[i + 1];
    size_t digits = 0;
    switch(e)
    {
      case '"':
      case '\\':
      case '/':
        out += e;
        break;
      case 'n':
        out += '\n';
        break;
      case 't':
        out += '\t';
        break;
      case 'r':
        out += '\r';
        break;
      case '0':
        out += '\0';
        break;
      case 'x':
        digits = 2;
        break;
      case 'u':
        digits = 4;
        break;
      case 'U':
        digits = 8;
        break;
      default:
        YamlError("bad escape", line);
    }
    i += 2;
    if(digits > 0)
    {
      const std::string hex = s.substr(i, digits);
      if(hex.size() != digits ||
         hex.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
      {
        YamlError("bad escape", line);
      }
      const auto cp = static_cast<uint32_t>(std::stoul(hex, nullptr, 16));
      if(cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
      {
        YamlError("bad escape", line);
      }
      AppendUtf8(out, cp);
      i += digits;
    }
  }
  YamlError("unterminated string", line);
}

inline std::string YamlScalar(const std::string& s, const std::string& line)
{
  if(!s.empty() && s.front() == '"')
  {
    std::string value;
    const size_t end = ReadQuoted(s, 0, value, line);
    if(s.find_first_not_of(' ', end) != std::string::npos)
    {
      YamlError("unexpected text after string", line);
    }
    return value;
  }
  if(s.empty() || std::string("'[{&*!|>-?").find(s.front()) != std::string::npos ||
     s.find(" #") != std::string::npos)
  {
    YamlError("unsupported YAML", line);
  }
  return s;
}

/// Splits "key: value" or "key:" into the key and the trimmed value text.
inline std::pair<std::string, std::string> SplitYamlEntry(const std::string& content,
                                                          const std::string& line)
{
  std::string key;
  std::string rest;
  if(content.front() == '"')
  {
    const size_t end = ReadQuoted(content, 0, key, line);
    if(end >= content.size() || content[end] != ':')
    {
      YamlError("expected ':' after key", line);
    }
    rest = content.substr(end + 1);
  }
  else
  {
    size_t colon = content.find(": ");
    if(colon == std::string::npos)
    {
      if(content.back() != ':')
      {
        YamlError("expected 'key: value'", line);
      }
      colon = content.size() - 1;
    }
    key = YamlScalar(content.substr(0, colon), line);
    rest = content.substr(colon + 1);
  }
  if(!rest.empty() && rest.front() != ' ')
  {
    YamlError("expected a space after ':'", line);
  }
  const auto first = rest.find_first_not_of(' ');
  rest = first == std::string::npos ? std::string() : rest.substr(first);
  return { key, rest };
}

/// Type names of the schema text, indexed by BasicType.
inline const std::array<std::string, TypesCount>& BasicTypeNames()
{
  static const std::array<std::string, TypesCount> kNames = {
    "bool",   "char",  "int8",   "uint8",   "int16",   "uint16", "int32",
    "uint32", "int64", "uint64", "float32", "float64", "other"
  };
  return kNames;
}

/// BasicType named exactly `name`; OTHER for anything else (custom types).
inline BasicType BasicTypeFromName(const std::string& name)
{
  const auto& names = BasicTypeNames();
  for(size_t i = 0; i + 1 < TypesCount; i++)
  {
    if(name == names[i])
    {
      return static_cast<BasicType>(i);
    }
  }
  return BasicType::OTHER;
}

/// Unsigned decimal, digits only. nullopt if invalid or above 2^64 - 1.
inline std::optional<uint64_t> ParseUnsigned(const std::string& value)
{
  if(value.empty() || value.size() > 20 ||
     value.find_first_not_of("0123456789") != std::string::npos)
  {
    return std::nullopt;
  }
  try
  {
    return std::stoull(value);
  }
  catch(const std::out_of_range&)
  {
    return std::nullopt;
  }
}

/// The N of a fixed-size array "[N]" (digits only). nullopt unless it is in 1..65535.
inline std::optional<uint16_t> ParseArrayExtent(const std::string& digits)
{
  const auto extent = ParseUnsigned(digits);
  if(!extent || *extent == 0 || *extent > 65535)
  {
    return std::nullopt;
  }
  return static_cast<uint16_t>(*extent);
}

/// Parses the block-mapping subset of YAML written by DataTamer::ToYaml().
inline std::vector<YamlNode> ParseYamlTree(const std::string& txt)
{
  struct Level
  {
    int parent_indent;
    std::optional<int> indent;  // indentation of this mapping's entries
    std::vector<YamlNode>* entries;
    std::unordered_set<std::string> keys;
  };
  std::vector<YamlNode> root;
  std::vector<Level> stack = { { -1, 0, &root, {} } };  // top level starts at column 0

  std::istringstream ss(txt);
  std::string line;
  while(std::getline(ss, line))
  {
    while(!line.empty() && (line.back() == ' ' || line.back() == '\r'))
    {
      line.pop_back();
    }
    const size_t first = line.find_first_not_of(' ');
    if(first == std::string::npos || line[first] == '#')
    {
      continue;
    }
    if(line[first] == '\t')
    {
      YamlError("tab indentation", line);
    }
    const int indent = int(first);
    while(indent <= stack.back().parent_indent)
    {
      stack.pop_back();
    }
    Level& level = stack.back();
    if(!level.indent)
    {
      level.indent = indent;
    }
    else if(*level.indent != indent)
    {
      YamlError("bad indentation", line);
    }
    auto [key, rest] = SplitYamlEntry(line.substr(first), line);
    if(!level.keys.insert(key).second)
    {
      YamlError("duplicate key", line);
    }
    YamlNode node;
    node.key = std::move(key);
    if(!rest.empty() && rest != "{}")
    {
      node.scalar = YamlScalar(rest, line);
    }
    level.entries->push_back(std::move(node));
    if(rest.empty())
    {
      if(stack.size() > kMaxYamlDepth)
      {
        YamlError("nesting too deep", line);
      }
      stack.push_back({ indent, std::nullopt, &level.entries->back().children, {} });
    }
  }
  return root;
}

inline TypeField ParseYamlTypeSpec(const std::string& spec)
{
  TypeField field;
  const auto bracket = spec.find('[');
  field.type_name = spec.substr(0, bracket);
  if(field.type_name.empty())
  {
    throw std::runtime_error("DataTamerParser: YAML schema: empty type");
  }
  field.type = BasicTypeFromName(field.type_name);
  if(bracket != std::string::npos)
  {
    field.is_vector = true;
    const std::string inner = spec.substr(bracket + 1, spec.size() - bracket - 2);
    if(spec.back() != ']' || inner.find_first_not_of("0123456789") != std::string::npos)
    {
      throw std::runtime_error("DataTamerParser: YAML schema: invalid type " + spec);
    }
    if(!inner.empty())
    {
      const auto extent = ParseArrayExtent(inner);
      if(!extent)
      {
        throw std::runtime_error("DataTamerParser: YAML schema: array size out of "
                                 "range (1..65535) in " +
                                 spec);
      }
      field.array_size = *extent;
    }
  }
  return field;
}

inline void FlattenYamlFields(const std::vector<YamlNode>& nodes,
                              const std::string& prefix, FieldsVector& out)
{
  for(const auto& node : nodes)
  {
    if(node.scalar)
    {
      TypeField field = ParseYamlTypeSpec(*node.scalar);
      field.field_name = prefix + node.key;
      out.push_back(std::move(field));
    }
    else
    {
      FlattenYamlFields(node.children, prefix + node.key + "/", out);
    }
  }
}

inline const YamlNode* FindYamlKey(const std::vector<YamlNode>& nodes,
                                   const std::string& key)
{
  for(const auto& node : nodes)
  {
    if(node.key == key)
    {
      return &node;
    }
  }
  return nullptr;
}
}  // namespace detail

/// Parses a YAML schema (version 6, docs/wire_format.md section 2.1).
/// BuildSchemaFromText() calls it when the text starts with "version:".
inline Schema BuildSchemaFromYaml(const std::string& txt, bool check_hash = false)
{
  using detail::FindYamlKey;
  const auto root = detail::ParseYamlTree(txt);
  auto scalar = [&](const std::string& key) -> const std::string& {
    const auto* node = FindYamlKey(root, key);
    if(!node || !node->scalar)
    {
      throw std::runtime_error("DataTamerParser: YAML schema: missing " + key);
    }
    return *node->scalar;
  };
  auto mapping = [&](const std::string& key, bool required) {
    const auto* node = FindYamlKey(root, key);
    if(node && node->scalar)
    {
      throw std::runtime_error("DataTamerParser: YAML schema: " + key +
                               " must be a mapping");
    }
    if(!node && required)
    {
      throw std::runtime_error("DataTamerParser: YAML schema: missing " + key);
    }
    return node ? &node->children : nullptr;
  };
  auto toUint = [](const std::string& value, const char* what) {
    const auto parsed = detail::ParseUnsigned(value);
    if(!parsed)
    {
      throw std::runtime_error(std::string("DataTamerParser: YAML schema: invalid ") +
                               what);
    }
    return *parsed;
  };

  if(toUint(scalar("version"), "version") != SCHEMA_YAML_VERSION)
  {
    throw std::runtime_error("Wrong SCHEMA_VERSION");
  }
  Schema schema;
  schema.hash = toUint(scalar("hash"), "hash");
  schema.channel_name = scalar("channel_name");
  detail::FlattenYamlFields(*mapping("fields", true), "", schema.fields);
  if(const auto* types = mapping("types", false))
  {
    for(const auto& type : *types)
    {
      if(type.scalar)
      {
        throw std::runtime_error("DataTamerParser: YAML schema: type " + type.key +
                                 " must be a mapping");
      }
      detail::FlattenYamlFields(type.children, "", schema.custom_types[type.key]);
    }
  }
  if(const auto* opaque = mapping("opaque_types", false))
  {
    for(const auto& type : *opaque)
    {
      const auto* encoding = FindYamlKey(type.children, "encoding");
      const auto* body = FindYamlKey(type.children, "schema");
      if(!encoding || !encoding->scalar || !body || !body->scalar)
      {
        throw std::runtime_error("DataTamerParser: YAML schema: opaque type " + type.key +
                                 " needs encoding and schema");
      }
      schema.custom_schemas[type.key] = { *encoding->scalar, *body->scalar };
    }
  }
  if(check_hash && schema.hash != SchemaTextHash(ToText(schema)))
  {
    throw std::runtime_error("Error in hash calculation");
  }
  return schema;
}

/// Parses a schema text in the line format (versions 4 and 5) or YAML (version 6), see
/// docs/wire_format.md section 2. Throws std::runtime_error on a malformed text or
/// another version; `check_hash` also throws if the declared hash is wrong.
inline Schema BuildSchemaFromText(const std::string& txt, bool check_hash = false)
{
  auto trimString = [](std::string& str) {
    while(!str.empty() && (str.back() == ' ' || str.back() == '\r'))
    {
      str.pop_back();
    }
    while(!str.empty() && (str.front() == ' ' || str.front() == '\r'))
    {
      str.erase(0, 1);
    }
  };

  {
    // The YAML rendering (version 6) starts with "version:"; the line format with "###".
    std::istringstream probe(txt);
    std::string first;
    while(std::getline(probe, first))
    {
      trimString(first);
      if(!first.empty() && first.front() != '#')
      {
        break;
      }
      if(first.rfind("###", 0) == 0)
      {
        break;
      }
    }
    if(first.rfind("version:", 0) == 0)
    {
      return BuildSchemaFromYaml(txt, check_hash);
    }
  }

  std::istringstream ss(txt);
  std::string line;
  Schema schema;
  uint64_t declared_schema = 0;
  int version = SCHEMA_VERSION;
  uint64_t legacy_hash = 0;  // version 4 recomputation, field by field

  std::vector<TypeField>* field_vector = &schema.fields;
  bool any_line = false;

  while(std::getline(ss, line))
  {
    trimString(line);
    if(line.empty())
    {
      continue;
    }
    any_line = true;
    if(line.find("==============================") != std::string::npos)
    {
      std::getline(ss, line);
      auto msg_pos = line.find("MSG: ");
      if(msg_pos == std::string::npos)
      {
        throw std::runtime_error("Expecting \"MSG: \" at the beginning of line: " + line);
      }
      line.erase(0, 5);
      trimString(line);
      field_vector = &schema.custom_types[line];
      continue;
    }

    // Headers: "### key: value", the value being what follows the colon.
    std::string header_value;
    auto isHeader = [&](const std::string& key) {
      const std::string prefix = "### " + key + ":";
      if(line.compare(0, prefix.size(), prefix) != 0)
      {
        return false;
      }
      header_value = line.substr(prefix.size());
      trimString(header_value);
      return true;
    };
    if(isHeader("version"))
    {
      const auto number = detail::ParseUnsigned(header_value);
      if(!number)
      {
        throw std::runtime_error("DataTamerParser: invalid version in: " + line);
      }
      // Version 4 differs only in how the hash was computed.
      if(*number != uint64_t(SCHEMA_VERSION) && *number != 4)
      {
        throw std::runtime_error("Wrong SCHEMA_VERSION");
      }
      version = int(*number);
      continue;
    }
    if(isHeader("hash"))
    {
      const auto number = detail::ParseUnsigned(header_value);
      if(!number)
      {
        throw std::runtime_error("DataTamerParser: invalid hash in: " + line);
      }
      declared_schema = *number;
      continue;
    }
    if(isHeader("channel_name"))
    {
      schema.channel_name = header_value;
      legacy_hash = std::hash<std::string>()(schema.channel_name);
      continue;
    }

    // Split at the first space: "<type> <name>".
    const auto space_pos = line.find(' ');
    if(space_pos == std::string::npos)
    {
      throw std::runtime_error("Unexpected line: " + line);
    }

    std::string str_left = line.substr(0, space_pos);
    std::string str_right = line.substr(space_pos + 1);
    trimString(str_left);
    trimString(str_right);

    const std::string* str_type = &str_left;
    const std::string* str_name = &str_right;

    TypeField field;

    const auto& kNamesNew = detail::BasicTypeNames();
    // Legacy files (before version 4): upper-case type names after the field name.
    static const std::array<std::string, TypesCount> kNamesOld = {
      "BOOL",   "CHAR",  "INT8",   "UINT8", "INT16",  "UINT16", "INT32",
      "UINT32", "INT64", "UINT64", "FLOAT", "DOUBLE", "OTHER"
    };

    // Compare the whole type token (before any "[...]"): "float64Pose" is a custom type.
    auto typeToken = [](const std::string& spec) {
      return spec.substr(0, spec.find('['));
    };
    for(size_t i = 0; i < TypesCount; i++)
    {
      if(typeToken(str_left) == kNamesNew[i])
      {
        field.type = static_cast<BasicType>(i);
        break;
      }
      if(typeToken(str_right) == kNamesOld[i])
      {
        field.type = static_cast<BasicType>(i);
        std::swap(str_type, str_name);
        break;
      }
    }

    auto offset = str_type->find_first_of(" [");
    if(field.type != BasicType::OTHER)
    {
      field.type_name = kNamesNew[static_cast<size_t>(field.type)];
    }
    else
    {
      field.type_name = str_type->substr(0, offset);
    }

    if(offset != std::string::npos && str_type->at(offset) == '[')
    {
      field.is_vector = true;
      auto pos = str_type->find(']', offset);
      if(pos == std::string::npos)
      {
        throw std::runtime_error("Unterminated array size in: " + line);
      }
      if(pos != offset + 1)
      {
        const std::string number_string = str_type->substr(offset + 1, pos - offset - 1);
        if(number_string.empty() ||
           number_string.find_first_not_of("0123456789") != std::string::npos)
        {
          throw std::runtime_error("Invalid array size in: " + line);
        }
        const auto extent = detail::ParseArrayExtent(number_string);
        if(!extent)
        {
          throw std::runtime_error("Array size out of range (1..65535) in: " + line);
        }
        field.array_size = *extent;
      }
    }

    field.field_name = *str_name;
    trimString(field.field_name);

    if(version == 4 && field_vector == &schema.fields)
    {
      legacy_hash = AddFieldToHash(field, legacy_hash);
    }
    field_vector->push_back(field);
  }
  if(!any_line)
  {
    throw std::runtime_error("DataTamerParser: empty schema text");
  }
  // The declared hash is the one snapshots carry. check_hash recomputes it with the
  // recipe of the text's version.
  const uint64_t computed = version == 4 ? legacy_hash : SchemaTextHash(txt);
  if(check_hash && declared_schema != 0 && declared_schema != computed)
  {
    throw std::runtime_error("Error in hash calculation");
  }
  schema.hash = declared_schema != 0 ? declared_schema : computed;
  return schema;
}

/// Old misspelled name, kept for one release so that existing parsers compile.
[[deprecated("use BuildSchemaFromText")]] inline Schema
BuilSchemaFromText(const std::string& txt, bool check_hash = false)
{
  return BuildSchemaFromText(txt, check_hash);
}

/// Wire size in bytes of a basic type (0 for OTHER).
inline size_t SizeOf(BasicType type)
{
  switch(type)
  {
    case BasicType::BOOL:
    case BasicType::CHAR:
    case BasicType::INT8:
    case BasicType::UINT8:
      return 1;
    case BasicType::INT16:
    case BasicType::UINT16:
      return 2;
    case BasicType::INT32:
    case BasicType::UINT32:
    case BasicType::FLOAT32:
      return 4;
    case BasicType::INT64:
    case BasicType::UINT64:
    case BasicType::FLOAT64:
      return 8;
    default:
      return 0;
  }
}

/// Nested custom types deeper than this are treated as a malformed (cyclic) schema.
constexpr int kMaxSchemaDepth = 64;

namespace detail
{
/// Fewest payload bytes one value of a custom type takes, by type name.
using MinSizes = std::map<std::string, size_t>;

/// Larger sizes saturate here, so that products of array extents cannot overflow.
constexpr size_t kHugeSize = std::numeric_limits<size_t>::max() / 2;

inline size_t MinFieldSize(const TypeField& field,
                           const std::map<std::string, FieldsVector>& types_list,
                           MinSizes& min_sizes, int depth);

/// Fewest payload bytes one element of `field` takes: a basic type's size, or the sum
/// of the fewest bytes of each field of the custom type.
inline size_t MinElementSize(const TypeField& field,
                             const std::map<std::string, FieldsVector>& types_list,
                             MinSizes& min_sizes, int depth)
{
  if(field.type != BasicType::OTHER)
  {
    return SizeOf(field.type);
  }
  const auto known = min_sizes.find(field.type_name);
  if(known != min_sizes.end())
  {
    return known->second;
  }
  if(depth > kMaxSchemaDepth)
  {
    throw std::runtime_error("DataTamerParser: custom types nested too deeply (cycle?)");
  }
  const auto type_it = types_list.find(field.type_name);
  if(type_it == types_list.end())
  {
    throw std::runtime_error("DataTamerParser: unknown type " + field.type_name);
  }
  size_t total = 0;
  for(const auto& sub_field : type_it->second)
  {
    const size_t size = MinFieldSize(sub_field, types_list, min_sizes, depth + 1);
    total = size > kHugeSize - total ? kHugeSize : total + size;
  }
  min_sizes.emplace(field.type_name, total);
  return total;
}

/// Fewest payload bytes `field` takes: its elements at their fewest bytes, or only the
/// 4 byte count for a dynamic vector.
inline size_t MinFieldSize(const TypeField& field,
                           const std::map<std::string, FieldsVector>& types_list,
                           MinSizes& min_sizes, int depth)
{
  if(field.is_vector && field.array_size == 0)
  {
    return sizeof(uint32_t);
  }
  const size_t element = MinElementSize(field, types_list, min_sizes, depth);
  const size_t count = field.is_vector ? field.array_size : 1;
  return (element != 0 && count > kHugeSize / element) ? kHugeSize : element * count;
}
}  // namespace detail

template <typename NumberCallback>
bool ParseSnapshotRecursive(const TypeField& field,
                            const std::map<std::string, FieldsVector>& types_list,
                            BufferSpan& buffer, const NumberCallback& callback_number,
                            const std::string& prefix, detail::MinSizes& min_sizes,
                            int depth = 0)
{
  if(depth > kMaxSchemaDepth)
  {
    throw std::runtime_error("DataTamerParser: custom types nested too deeply (cycle?)");
  }
  uint32_t vect_size = field.array_size;
  if(field.is_vector && field.array_size == 0)
  {
    vect_size = Deserialize<uint32_t>(buffer);
  }
  if(field.is_vector && vect_size > 0)
  {
    // An element takes at least min_size bytes: a count the payload cannot hold is
    // rejected. Elements of no byte hold no value, however many there are.
    const size_t min_size = detail::MinElementSize(field, types_list, min_sizes, depth);
    if(min_size == 0)
    {
      return true;
    }
    if(vect_size > buffer.size / min_size)
    {
      throw std::runtime_error("DataTamerParser: payload truncated");
    }
  }

  auto new_prefix =
      (prefix.empty()) ? field.field_name : (prefix + "/" + field.field_name);

  auto doParse = [&](const std::string& var_name) {
    if(field.type != BasicType::OTHER)
    {
      const auto var = DeserializeToVarNumber(field.type, buffer);
      callback_number(var_name, var);
    }
    else
    {
      const auto type_it = types_list.find(field.type_name);
      if(type_it == types_list.end())
      {
        throw std::runtime_error("DataTamerParser: unknown type " + field.type_name);
      }
      for(const auto& sub_field : type_it->second)
      {
        ParseSnapshotRecursive(sub_field, types_list, buffer, callback_number, var_name,
                               min_sizes, depth + 1);
      }
    }
  };

  if(!field.is_vector)
  {
    doParse(new_prefix);
  }
  else
  {
    for(uint32_t a = 0; a < vect_size; a++)
    {
      const auto& name = new_prefix + "[" + std::to_string(a) + "]";
      doParse(name);
    }
  }
  return true;
}

template <typename NumberCallback, typename CustomCallback>
inline bool ParseSnapshot(const Schema& schema, SnapshotView snapshot,
                          const NumberCallback& callback_number,
                          const CustomCallback& callback_custom)
{
  if(schema.hash != snapshot.schema_hash)
  {
    return false;
  }
  BufferSpan buffer = snapshot.payload;
  detail::MinSizes min_sizes;
  if(snapshot.active_mask.size * 8 < schema.fields.size())
  {
    throw std::runtime_error("DataTamerParser: active mask shorter than the schema");
  }

  for(size_t i = 0; i < schema.fields.size(); i++)
  {
    const auto& field = schema.fields[i];
    if(GetBit(snapshot.active_mask, i))
    {
      ParseSnapshotRecursive(field, schema.custom_types, buffer, callback_number, "",
                             min_sizes);
    }
  }
  // leftover bytes: the schema and the payload do not belong together
  return buffer.size == 0;
}

inline const Schema& SchemaRegistry::add(uint64_t hash, const std::string& schema_text)
{
  auto it = schemas_.find(hash);
  if(it == schemas_.end())
  {
    Schema schema = BuildSchemaFromText(schema_text);
    if(schema.hash != hash)
    {
      // snapshots carry `hash`: a schema declaring another one would never match them
      throw std::runtime_error("DataTamerParser: schema text declares hash " +
                               std::to_string(schema.hash) + ", expected " +
                               std::to_string(hash));
    }
    it = schemas_.emplace(hash, std::move(schema)).first;
  }
  return it->second;
}

template <typename SchemasMsgT>
inline void SchemaRegistry::addSchemas(const SchemasMsgT& msg)
{
  for(const auto& schema_msg : msg.schemas)
  {
    add(schema_msg.hash, schema_msg.schema_text);
  }
}

inline const Schema* SchemaRegistry::find(uint64_t hash) const
{
  auto it = schemas_.find(hash);
  return it == schemas_.end() ? nullptr : &it->second;
}

template <typename SnapshotMsgT>
inline SnapshotView ToSnapshotView(const SnapshotMsgT& msg)
{
  SnapshotView view;
  view.schema_hash = msg.schema_hash;
  view.timestamp = msg.timestamp_nsec;
  view.active_mask = { msg.active_mask.data(), msg.active_mask.size() };
  view.payload = { msg.payload.data(), msg.payload.size() };
  return view;
}

template <typename BatchMsgT, typename SnapshotCallback>
inline size_t ForEachSnapshotInBatch(SchemaRegistry& registry, const BatchMsgT& batch,
                                     const SnapshotCallback& callback)
{
  registry.addSchemas(batch);
  size_t visited = 0;
  for(const auto& snapshot_msg : batch.snapshots)
  {
    if(const Schema* schema = registry.find(snapshot_msg.schema_hash))
    {
      callback(*schema, ToSnapshotView(snapshot_msg));
      visited++;
    }
  }
  return visited;
}

}  // namespace DataTamerParser
