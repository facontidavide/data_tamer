#pragma once

#include <string_view>
#include <vector>

namespace DataTamerTest
{

/// Custom types for the tests of registration: a flat one, and one that holds another
/// type directly and in a vector.
struct Reading
{
  double value = 0;
};
template <typename AddField>
std::string_view TypeDefinition(Reading& reading, AddField& add)
{
  add("value", &reading.value);
  return "Reading";
}

struct Inner
{
  double a = 0;
};
template <typename AddField>
std::string_view TypeDefinition(Inner& inner, AddField& add)
{
  add("a", &inner.a);
  return "Inner";
}

struct Outer
{
  Inner first;
  std::vector<Inner> rest;
};
template <typename AddField>
std::string_view TypeDefinition(Outer& outer, AddField& add)
{
  add("first", &outer.first);
  add("rest", &outer.rest);
  return "Outer";
}

}  // namespace DataTamerTest
