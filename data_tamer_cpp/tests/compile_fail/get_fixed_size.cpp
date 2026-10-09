// Compile-fail probe (see data_tamer_compile_fail_test in tests/CMakeLists.txt).
// GetFixedSize() walks the fields of a custom type, so the type needs a TypeDefinition.
// As written the probe sizes a described type; DT_UNDESCRIBED_TYPE sizes one without.
#include "data_tamer/custom_types.hpp"
#include "described_type.hpp"

namespace probe
{
struct Undescribed
{
  int a = 0;
};

size_t fixedSizeOfDescribed()
{
  bool fixed = true;
  size_t size = 0;
  DataTamer::GetFixedSize<Described>(fixed, size);
#ifdef DT_UNDESCRIBED_TYPE
  DataTamer::GetFixedSize<Undescribed>(fixed, size);
#endif
  return size;
}
}  // namespace probe
