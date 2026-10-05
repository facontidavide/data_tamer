#include "data_tamer/types.hpp"
#include <array>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <locale>
#include <set>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

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

std::string RenderSchema(const Schema& schema, SchemaFormat format)
{
  return format == SchemaFormat::Yaml ? ToYaml(schema) : ToStr(schema);
}

uint64_t ComputeSchemaHash(const Schema& schema)
{
  return SchemaTextHash(ToStr(schema));
}

namespace
{
/// "type", "type[]" or "type[N]": the only renderer of a field's type, shared by
/// the line format (which the schema hash is computed on) and ToYaml().
std::string TypeSpec(const TypeField& field)
{
  std::string spec = field.type == BasicType::OTHER ? field.type_name : ToStr(field.type);
  if(field.is_vector)
  {
    spec += field.array_size == 0 ? "[]" : "[" + std::to_string(field.array_size) + "]";
  }
  return spec;
}
}  // namespace

std::ostream& operator<<(std::ostream& os, const TypeField& field)
{
  os << TypeSpec(field) << ' ' << field.field_name;
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
  ss.imbue(std::locale::classic());  // the hash covers this text: never locale-dependent
  ss << schema;
  return ss.str();
}

//------------------------------------------------------------------
// YAML rendering (docs/wire_format.md, section 2.1)

namespace
{
bool IsAlpha(char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
bool IsDigit(char c)
{
  return c >= '0' && c <= '9';
}

// Words that a YAML 1.1 loader would read as a boolean or null.
bool IsReservedWord(std::string_view s)
{
  static const std::array<std::string_view, 9> kReserved = { "y",    "n",     "yes",
                                                             "no",   "on",    "off",
                                                             "true", "false", "null" };
  if(s.size() > 5)
  {
    return false;
  }
  std::string lower(s);
  for(auto& c : lower)
  {
    if(c >= 'A' && c <= 'Z')
    {
      c = char(c - 'A' + 'a');
    }
  }
  for(const auto& word : kReserved)
  {
    if(lower == word)
    {
      return true;
    }
  }
  return false;
}

// Plain (unquoted) keys and names: [A-Za-z_][A-Za-z0-9_./-]*
bool IsPlainName(std::string_view s)
{
  if(s.empty() || !IsAlpha(s.front()) || IsReservedWord(s))
  {
    return false;
  }
  for(const char c : s)
  {
    if(!IsAlpha(c) && !IsDigit(c) && c != '.' && c != '/' && c != '-')
    {
      return false;
    }
  }
  return true;
}

// Plain type specs: [A-Za-z_][A-Za-z0-9_]* optionally followed by [] or [N]
bool IsPlainTypeSpec(std::string_view s)
{
  const auto bracket = s.find('[');
  const auto type = s.substr(0, bracket);
  if(type.empty() || !IsAlpha(type.front()) || IsReservedWord(type))
  {
    return false;
  }
  for(const char c : type)
  {
    if(!IsAlpha(c) && !IsDigit(c))
    {
      return false;
    }
  }
  if(bracket == std::string_view::npos)
  {
    return true;
  }
  if(s.back() != ']')
  {
    return false;
  }
  for(size_t i = bracket + 1; i + 1 < s.size(); i++)
  {
    if(!IsDigit(s[i]))
    {
      return false;
    }
  }
  return true;
}

void WriteQuoted(std::ostream& os, std::string_view s)
{
  static const char* kHex = "0123456789ABCDEF";
  os << '"';
  for(size_t i = 0; i < s.size(); i++)
  {
    const char c = s[i];
    const auto byte = static_cast<uint8_t>(c);
    // YAML 1.1 loaders treat NEL, LS and PS as line breaks even inside quotes
    if(s.substr(i, 2) == "\xC2\x85")
    {
      os << "\\x85";
      i += 1;
      continue;
    }
    if(s.substr(i, 3) == "\xE2\x80\xA8" || s.substr(i, 3) == "\xE2\x80\xA9")
    {
      os << (s[i + 2] == '\xA8' ? "\\u2028" : "\\u2029");
      i += 2;
      continue;
    }
    switch(c)
    {
      case '"':
        os << "\\\"";
        break;
      case '\\':
        os << "\\\\";
        break;
      case '\n':
        os << "\\n";
        break;
      case '\t':
        os << "\\t";
        break;
      case '\r':
        os << "\\r";
        break;
      default:
        if(byte < 0x20 || byte == 0x7f)
        {
          os << "\\x" << kHex[byte >> 4] << kHex[byte & 0xf];
        }
        else
        {
          os << c;
        }
    }
  }
  os << '"';
}

void WriteName(std::ostream& os, std::string_view s)
{
  if(IsPlainName(s))
  {
    os << s;
  }
  else
  {
    WriteQuoted(os, s);
  }
}

void Indent(std::ostream& os, int indent)
{
  os << std::string(static_cast<size_t>(indent), ' ');
}

using NamedField = std::pair<std::string_view, const TypeField*>;

// Writes the entries of one mapping. A run of two or more consecutive fields
// whose names start with "head/" becomes a nested mapping under "head", unless
// that key is already taken in this mapping (by a field named "head", or by an
// earlier run); such fields keep their relative name as a flat key. Field
// order is preserved, which the payload layout requires. Below kMaxNesting
// levels, runs are not nested any further.
constexpr int kMaxNesting = 16;

void WriteEntries(std::ostream& os, const std::vector<NamedField>& items, int indent,
                  int depth)
{
  std::set<std::string_view> taken;
  for(const auto& [name, field] : items)
  {
    if(name.find('/') == std::string_view::npos)
    {
      taken.insert(name);
    }
  }

  auto writeLeaf = [&](const NamedField& item) {
    Indent(os, indent);
    WriteName(os, item.first);
    os << ": ";
    const auto spec = TypeSpec(*item.second);
    if(IsPlainTypeSpec(spec))
    {
      os << spec;
    }
    else
    {
      WriteQuoted(os, spec);
    }
    os << "\n";
  };

  size_t i = 0;
  while(i < items.size())
  {
    const auto name = items[i].first;
    const auto slash = name.find('/');
    if(slash == std::string_view::npos)
    {
      writeLeaf(items[i++]);
      continue;
    }
    const auto head = name.substr(0, slash + 1);  // including the '/'
    size_t j = i + 1;
    while(j < items.size() && items[j].first.substr(0, head.size()) == head)
    {
      j++;
    }
    const auto key = head.substr(0, slash);
    if(j - i >= 2 && depth < kMaxNesting && !key.empty() && taken.insert(key).second)
    {
      Indent(os, indent);
      WriteName(os, key);
      os << ":\n";
      std::vector<NamedField> children;
      for(size_t k = i; k < j; k++)
      {
        children.emplace_back(items[k].first.substr(head.size()), items[k].second);
      }
      WriteEntries(os, children, indent + 2, depth + 1);
    }
    else
    {
      for(size_t k = i; k < j; k++)
      {
        writeLeaf(items[k]);
      }
    }
    i = j;
  }
}

void WriteFields(std::ostream& os, const FieldsVector& fields, int indent)
{
  if(fields.empty())
  {
    os << " {}\n";
    return;
  }
  os << "\n";
  std::vector<NamedField> items;
  items.reserve(fields.size());
  for(const auto& field : fields)
  {
    items.emplace_back(field.field_name, &field);
  }
  WriteEntries(os, items, indent, 0);
}
}  // namespace

std::string ToYaml(const Schema& schema)
{
  std::ostringstream os;
  os.imbue(std::locale::classic());  // the hash is written here: never locale-dependent
  os << "version: " << SCHEMA_YAML_VERSION << "\n";
  os << "hash: " << schema.hash << "\n";
  os << "channel_name: ";
  WriteName(os, schema.channel_name);
  os << "\nfields:";
  WriteFields(os, schema.fields, 2);
  if(!schema.custom_types.empty())
  {
    os << "types:\n";
    for(const auto& [type_name, custom_fields] : schema.custom_types)
    {
      Indent(os, 2);
      WriteName(os, type_name);
      os << ":";
      WriteFields(os, custom_fields, 4);
    }
  }
  if(!schema.custom_schemas.empty())
  {
    os << "opaque_types:\n";
    for(const auto& [type_name, custom_schema] : schema.custom_schemas)
    {
      Indent(os, 2);
      WriteName(os, type_name);
      os << ":\n    encoding: ";
      WriteName(os, custom_schema.encoding);
      os << "\n    schema: ";
      WriteQuoted(os, custom_schema.schema);
      os << "\n";
    }
  }
  return os.str();
}

}  // namespace DataTamer
