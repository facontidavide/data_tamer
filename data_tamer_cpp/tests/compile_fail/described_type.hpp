// A custom type with a TypeDefinition, for the compile-fail probes that need one.
#pragma once

#include <string_view>

namespace probe
{
struct Described
{
  int a = 0;
};

template <typename AddField>
std::string_view TypeDefinition(Described& described, AddField& add)
{
  add("a", &described.a);
  return "Described";
}
}  // namespace probe
