// Compile-fail probe (see data_tamer_compile_fail_test in tests/CMakeLists.txt).
// A type name is not a serializer: CustomSerializerT must be built from it explicitly.
// As written the serializer is built with the name directly; DT_IMPLICIT_NAME converts
// a std::string into one.
#include "data_tamer/custom_types.hpp"

#include <memory>
#include <string>
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

std::shared_ptr<DataTamer::CustomSerializer> make()
{
  DataTamer::CustomSerializerT<Described> direct(std::string("Described"));
#ifdef DT_IMPLICIT_NAME
  DataTamer::CustomSerializerT<Described> implicit = std::string("Described");
  (void)implicit;
#endif
  return std::make_shared<DataTamer::CustomSerializerT<Described>>(std::string("Named"));
}
}  // namespace probe
