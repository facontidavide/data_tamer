// Compile-fail probe (see data_tamer_compile_fail_test in tests/CMakeLists.txt).
// long double has no wire type, so registering one must be refused with a clear
// message. As written the probe registers doubles; each DT_LONG_DOUBLE_* macro turns
// one registration into a long double.
#include "data_tamer/channel.hpp"

#include <string_view>
#include <vector>

#ifdef DT_LONG_DOUBLE_VALUE
using Scalar = long double;
#else
using Scalar = double;
#endif
#ifdef DT_LONG_DOUBLE_VECTOR
using Element = long double;
#else
using Element = double;
#endif
#ifdef DT_LONG_DOUBLE_MEMBER
using Member = long double;
#else
using Member = double;
#endif
#ifdef DT_LONG_DOUBLE_LOGGED
using Logged = long double;
#else
using Logged = double;
#endif

struct Holder
{
  Member member = 0;
};

template <typename AddField>
std::string_view TypeDefinition(Holder& holder, AddField& add)
{
  add("member", &holder.member);
  return "Holder";
}

void probe(DataTamer::LogChannel& channel)
{
  Scalar scalar = 0;
  std::vector<Element> vector;
  Holder holder;
  channel.registerValue("scalar", &scalar);
  channel.registerValue("vector", &vector);
  channel.registerValue("holder", &holder);
  [[maybe_unused]] auto logged = channel.createLoggedValue<Logged>("logged");
}
