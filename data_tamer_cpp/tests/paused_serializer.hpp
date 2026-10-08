#pragma once

#include "data_tamer/custom_types.hpp"
#include "gate.hpp"

#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>

namespace DataTamerTest
{

/// The value a PausedSerializer writes: 8 bytes.
struct CustomValue
{
  uint64_t value = 42;
};

/// Serializer of a CustomValue for the tests of a snapshot in progress. While `gate` is
/// set, serializedSize() parks the snapshot on it until the test opens the gate; the
/// throw_* flags make the calls throw, and size_calls counts serializedSize().
class PausedSerializer : public DataTamer::CustomSerializer
{
public:
  mutable Gate* gate = nullptr;
  bool throw_size = false;
  bool throw_serialize = false;
  bool throw_schema = false;
  mutable size_t size_calls = 0;

  std::optional<DataTamer::CustomSchema> typeSchema() const override
  {
    if(throw_schema)
    {
      throw std::runtime_error("schema");
    }
    return std::nullopt;
  }
  const std::string& typeName() const override
  {
    static const std::string name = "CustomValue";
    return name;
  }
  bool isFixedSize() const override { return true; }
  size_t serializedSize(const void*) const override
  {
    ++size_calls;
    if(gate)
    {
      gate->pause();
    }
    if(throw_size)
    {
      throw std::runtime_error("size");
    }
    return 8;
  }
  void serialize(const void* source, SerializeMe::SpanBytes& bytes) const override
  {
    if(throw_serialize)
    {
      throw std::runtime_error("serialize");
    }
    const auto value = static_cast<const CustomValue*>(source)->value;
    std::memcpy(bytes.data(), &value, 8);
    bytes.trimFront(8);
  }
};

}  // namespace DataTamerTest
