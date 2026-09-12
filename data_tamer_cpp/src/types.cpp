#include "data_tamer/types.hpp"
#include <array>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <unordered_map>

namespace DataTamer
{

// clang-format off
static const std::array<std::string, TypesCount> kNames = {
    "bool", "char",
    "int8", "uint8",
    "int16", "uint16",
    "int32", "uint32",
    "int64", "uint64",
    "float32", "float64",
    "other"
};
// clang-format on

const std::string& ToStr(const BasicType& type)
{
  return kNames[static_cast<size_t>(type)];
}

BasicType FromStr(const std::string& str)
{
  static const auto kMap = []() {
    std::unordered_map<std::string, BasicType> map;
    for(size_t i = 0; i < TypesCount; i++)
    {
      auto type = static_cast<BasicType>(i);
      map[ToStr(type)] = type;
    }
    return map;
  }();

  auto const it = kMap.find(str);
  return it == kMap.end() ? BasicType::OTHER : it->second;
}

size_t SizeOf(const BasicType& type)
{
  // clang-format off
  static constexpr std::array<size_t, TypesCount> kSizes =
      { 1, 1,
        1, 1,
        2, 2, 4, 4, 8, 8,
        4, 8, 0 };
  // clang-format on
  return kSizes[static_cast<size_t>(type)];
}

template <typename T>
T DeserializeImpl(const void* data)
{
  T var;
  std::memcpy(&var, data, sizeof(T));
  return var;
}

VarNumber DeserializeAsVarType(const BasicType& type, const void* data)
{
  // clang-format off
  switch(type)
  {
    case BasicType::BOOL: return DeserializeImpl<bool>(data);
    case BasicType::CHAR: return DeserializeImpl<char>(data);

    case BasicType::INT8: return DeserializeImpl<int8_t>(data);
    case BasicType::UINT8: return DeserializeImpl<uint8_t>(data);

    case BasicType::INT16: return DeserializeImpl<int16_t>(data);
    case BasicType::UINT16: return DeserializeImpl<uint16_t>(data);

    case BasicType::INT32: return DeserializeImpl<int32_t>(data);
    case BasicType::UINT32: return DeserializeImpl<uint32_t>(data);

    case BasicType::INT64: return DeserializeImpl<int64_t>(data);
    case BasicType::UINT64: return DeserializeImpl<uint64_t>(data);

    case BasicType::FLOAT32: return DeserializeImpl<float>(data);
    case BasicType::FLOAT64: return DeserializeImpl<double>(data);

    case BasicType::OTHER:
      return double(std::numeric_limits<double>::quiet_NaN());
  }
  // clang-format on
  return {};
}

uint64_t SchemaTextHash(std::string_view text)
{
  uint64_t hash = 0xcbf29ce484222325ULL;  // FNV-1a 64, see docs/wire_format.md
  auto feed = [&hash](std::string_view bytes) {
    for(const char c : bytes)
    {
      hash ^= static_cast<uint8_t>(c);
      hash *= 0x100000001b3ULL;
    }
  };
  const auto hash_line = text.find("### hash:");
  if(hash_line == std::string_view::npos)
  {
    feed(text);
    return hash;
  }
  const auto line_end = text.find('\n', hash_line);
  feed(text.substr(0, hash_line));
  if(line_end != std::string_view::npos)
  {
    feed(text.substr(line_end + 1));
  }
  return hash;
}

uint64_t ComputeSchemaHash(const Schema& schema)
{
  return SchemaTextHash(ToStr(schema));
}

std::ostream& operator<<(std::ostream& os, const TypeField& field)
{
  if(field.type == BasicType::OTHER)
  {
    os << field.type_name;
  }
  else
  {
    os << ToStr(field.type);
  }

  if(field.is_vector && field.array_size != 0)
  {
    os << "[" << field.array_size << "]";
  }
  if(field.is_vector && field.array_size == 0)
  {
    os << "[]";
  }
  os << ' ' << field.field_name;
  return os;
}

std::ostream& operator<<(std::ostream& os, const Schema& schema)
{
  os << "### version: " << SCHEMA_VERSION << "\n";
  os << "### hash: " << schema.hash << "\n";
  os << "### channel_name: " << schema.channel_name << "\n\n";

  //  std::map<std::string, CustomSerializer::Ptr> custom_types;
  for(const auto& field : schema.fields)
  {
    os << field << "\n";
  }

  for(const auto& [type_name, custom_fields] : schema.custom_types)
  {
    os << "===========================================================\n"
       << "MSG: " << type_name << "\n";
    for(const auto& field : custom_fields)
    {
      os << field << "\n";
    }
  }
  for(const auto& [type_name, custom_schema] : schema.custom_schemas)
  {
    os << "===========================================================\n"
       << "MSG: " << type_name << "\n"
       << "ENCODING: " << custom_schema.encoding << "\n"
       << custom_schema.schema << "\n";
  }

  return os;
}

bool TypeField::operator==(const TypeField& other) const
{
  return is_vector == other.is_vector && type == other.type &&
         array_size == other.array_size && field_name == other.field_name &&
         type_name == other.type_name;
}

bool TypeField::operator!=(const TypeField& other) const
{
  return !(*this == other);
}

std::string ToStr(const Schema& schema)
{
  std::ostringstream ss;
  ss << schema;
  return ss.str();
}

}  // namespace DataTamer
