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
#include <unordered_map>
#include <variant>
#include <vector>

namespace DataTamerParser
{

constexpr int SCHEMA_VERSION = 5;

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

struct BufferSpan
{
  const uint8_t* data = nullptr;
  size_t size = 0;

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

/**
 * @brief DataTamer uses a simple "flat" schema of key/value pairs (each pair is a "field").
 */
struct Schema
{
  uint64_t hash = 0;
  FieldsVector fields;
  std::string channel_name;

  std::map<std::string, FieldsVector> custom_types;
};

struct SnapshotView
{
  /// Unique identifier of the schema
  uint64_t schema_hash;

  /// snapshot timestamp
  uint64_t timestamp;

  /// Vector that tell us if a field of the schema is
  /// active or not. It is basically an optimized vector
  /// of bools, where each byte contains 8 boolean flags.
  BufferSpan active_mask;

  /// serialized data containing all the values, ordered as in the schema
  BufferSpan payload;
};

bool GetBit(BufferSpan mask, size_t index);

constexpr auto NullCustomCallback = [](const std::string&, const BufferSpan,
                                       const std::string&) {};

// Callback must be a std::function or lambda with signature:
//
// void(const std::string& name_field, const VarNumber& value)
//
// void(const std::string& name_field, const BufferSpan payload, const std::string& type_name)
//
template <typename NumberCallback, typename CustomCallback = decltype(NullCustomCallback)>
bool ParseSnapshot(const Schema& schema, SnapshotView snapshot,
                   const NumberCallback& callback_number,
                   const CustomCallback& callback_custom = NullCustomCallback);

//---------------------------------------------------------
//---------------------------------------------------------
//---------------------------------------------------------

template <typename T>
inline T Deserialize(BufferSpan& buffer)
{
  T var;
  const auto N = sizeof(T);
  if(N > buffer.size)
  {
    throw std::runtime_error("DataTamerParser: payload truncated");
  }
  std::memcpy(&var, buffer.data, N);
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

/// Hash recipe of schema version 4 (std::hash based, so only reproducible on the
/// writer's platform). Kept so that version 4 texts are read and verified exactly
/// as before.
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
/// "### hash:" line (wire format, section 5). Platform independent.
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

inline Schema BuilSchemaFromText(const std::string& txt, bool check_hash = false)
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

  std::istringstream ss(txt);
  std::string line;
  Schema schema;
  uint64_t declared_schema = 0;
  int version = SCHEMA_VERSION;
  uint64_t legacy_hash = 0;  // version 4 recomputation, field by field

  std::vector<TypeField>* field_vector = &schema.fields;

  while(std::getline(ss, line))
  {
    trimString(line);
    if(line.empty())
    {
      continue;
    }
    if(line.find("==============================") != std::string::npos)
    {
      // get "MSG:" in the next line
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

    // a single space is expected
    auto space_pos = line.find(' ');
    if(space_pos == std::string::npos)
    {
      throw std::runtime_error("Unexpected line: " + line);
    }
    if(line.find("### ") == 0)
    {
      space_pos = line.find(' ', 5);
    }

    std::string str_left = line.substr(0, space_pos);
    std::string str_right = line.substr(space_pos + 1, line.size() - (space_pos + 1));
    trimString(str_left);
    trimString(str_right);

    const std::string* str_type = &str_left;
    const std::string* str_name = &str_right;

    if(str_left == "### version:")
    {
      // Version 4 differs only in how the hash was computed.
      version = std::stoi(str_right);
      if(version != SCHEMA_VERSION && version != 4)
      {
        throw std::runtime_error("Wrong SCHEMA_VERSION");
      }
      continue;
    }
    if(str_left == "### hash:")
    {
      // check compatibility
      declared_schema = std::stoull(str_right);
      continue;
    }

    if(str_left == "### channel_name:")
    {
      // check compatibility
      schema.channel_name = str_right;
      legacy_hash = std::hash<std::string>()(schema.channel_name);
      continue;
    }

    TypeField field;

    static const std::array<std::string, TypesCount> kNamesNew = {
      "bool",   "char",  "int8",   "uint8",   "int16",   "uint16", "int32",
      "uint32", "int64", "uint64", "float32", "float64", "other"
    };
    // backcompatibility to old format
    static const std::array<std::string, TypesCount> kNamesOld = {
      "BOOL",   "CHAR",  "INT8",   "UINT8", "INT16",  "UINT16", "INT32",
      "UINT32", "INT64", "UINT64", "FLOAT", "DOUBLE", "OTHER"
    };

    // The type token is everything before an optional "[...]": compare it exactly,
    // otherwise a custom type named e.g. "float64Pose" would parse as float64.
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
        const unsigned long long extent = std::stoull(number_string);
        if(extent == 0 || extent > 65535)
        {
          throw std::runtime_error("Array size out of range (1..65535) in: " + line);
        }
        field.array_size = static_cast<uint16_t>(extent);
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
  // Snapshots carry the writer's declared hash: that is what to match against.
  // check_hash verifies it against the recomputation of the text's own version
  // (version 4's std::hash recipe only agrees on the writer's platform).
  const uint64_t computed = version == 4 ? legacy_hash : SchemaTextHash(txt);
  if(check_hash && declared_schema != 0 && declared_schema != computed)
  {
    throw std::runtime_error("Error in hash calculation");
  }
  schema.hash = declared_schema != 0 ? declared_schema : computed;
  return schema;
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

template <typename NumberCallback>
bool ParseSnapshotRecursive(const TypeField& field,
                            const std::map<std::string, FieldsVector>& types_list,
                            BufferSpan& buffer, const NumberCallback& callback_number,
                            const std::string& prefix, int depth = 0)
{
  if(depth > kMaxSchemaDepth)
  {
    throw std::runtime_error("DataTamerParser: custom types nested too deeply (cycle?)");
  }
  uint32_t vect_size = field.array_size;
  if(field.is_vector && field.array_size == 0)
  {
    // dynamic vector: the count cannot exceed what the payload can hold
    vect_size = Deserialize<uint32_t>(buffer);
    if(field.type != BasicType::OTHER &&
       size_t(vect_size) * SizeOf(field.type) > buffer.size)
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
                               depth + 1);
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
  if(snapshot.active_mask.size * 8 < schema.fields.size())
  {
    throw std::runtime_error("DataTamerParser: active mask shorter than the schema");
  }

  for(size_t i = 0; i < schema.fields.size(); i++)
  {
    const auto& field = schema.fields[i];
    if(GetBit(snapshot.active_mask, i))
    {
      ParseSnapshotRecursive(field, schema.custom_types, buffer, callback_number, "");
    }
  }
  // every enabled field consumed exactly its bytes; leftovers mean schema/payload mismatch
  return buffer.size == 0;
}

}  // namespace DataTamerParser
