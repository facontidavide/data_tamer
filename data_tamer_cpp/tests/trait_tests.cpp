#include "data_tamer/contrib/SerializeMe.hpp"
#include <gtest/gtest.h>
#include <vector>
#include <array>

using namespace SerializeMe;

// non-template type with TypeDefinition
struct CustomWithTypeDef
{
  int a;
  double b;
};

template <typename AddField>
std::string_view TypeDefinition(CustomWithTypeDef&, AddField&)
{
  return "CustomWithTypeDef";
}

// non-template type without TypeDefinition
struct CustomNoTypeDef
{
  int x;
};

// a template but non-variadic custom type
template <typename T>
struct TemplateCustomType
{
  T type;
};

template <typename T, typename AddField>
std::string_view TypeDefinition(TemplateCustomType<T>&, AddField&)
{
  return "TemplateCustomType";
}

// type with variadic parameters but *not* a container type
template <typename... Args>
struct VariadicCustomType
{
  int id;
  std::tuple<Args...> values;
};

template <typename... Args, typename AddField>
std::string_view TypeDefinition(VariadicCustomType<Args...>&, AddField&)
{
  return "VariadicCustomType";
}

// check that all the traits work as expected at compile time
// the below tests run, but are here primarily to ensure compilation works as intended for user-defined types
// the static asserts are the crux of the test
static_assert(has_TypeDefinition<CustomWithTypeDef>::value, "CustomWithTypeDef should "
                                                            "have TypeDefinition");
static_assert(!has_TypeDefinition<CustomNoTypeDef>::value, "CustomNoTypeDef should not "
                                                           "have TypeDefinition");
static_assert(!has_TypeDefinition<std::vector<int>>::value, "std::vector<int> should not "
                                                            "have TypeDefinition");
static_assert(!has_TypeDefinition<std::array<double, 3>>::value, "std::array<double, 3> "
                                                                 "should not have "
                                                                 "TypeDefinition");
static_assert(has_TypeDefinition<VariadicCustomType<int, double>>::value, "MyCustomType<"
                                                                          "int, "
                                                                          "double> "
                                                                          "should have "
                                                                          "TypeDefinitio"
                                                                          "n");

static_assert(has_TypeDefinition<TemplateCustomType<int>>::value, "TemplateCustomType<"
                                                                  "int> should have "
                                                                  "TypeDefinition");

// check that BufferSize works on the types
TEST(CustomTypeTrait, BufferSize)
{
  // a container of a type without TypeDefinition does not compile either: it cannot
  // be serialized, so it has no size

  // std containers of std types
  std::vector<int> v{ 1, 2, 3 };
  EXPECT_NO_THROW((BufferSize(v)));
  std::array<double, 2> arr{ { 4.0, 5.0 } };
  EXPECT_NO_THROW((BufferSize(arr)));

  // template custom type
  TemplateCustomType<int> template_type_def_type{ 31415 };
  EXPECT_NO_THROW((BufferSize(template_type_def_type)));

  // variadic custom type
  VariadicCustomType<int, double> variadic_type_def_type{ 1, std::make_tuple(2, 3.) };
  EXPECT_NO_THROW((BufferSize(variadic_type_def_type)));

  // container overload for custom type with TypeDefinition
  std::vector<CustomWithTypeDef> vector_of_type_def{ { 1, 2. }, { 3, 4. } };
  EXPECT_NO_THROW((BufferSize(vector_of_type_def)));

  // non-variadic custom types
  CustomWithTypeDef type_def_type{};
  EXPECT_NO_THROW((BufferSize(type_def_type)));
  CustomNoTypeDef no_type_def{};
  // this wouldn't compile since it isn't in a container and has no type def
  // BufferSize(no_type_def);
}
