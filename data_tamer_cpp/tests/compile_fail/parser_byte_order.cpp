// Compile-fail probe (see data_tamer_compile_fail_test in tests/CMakeLists.txt).
// The parser reads numbers with a plain memcpy, so it needs a little endian host: the
// failing variant redefines __BYTE_ORDER__ as big endian and must not compile.
#include "data_tamer_parser/data_tamer_parser.hpp"

uint32_t countOf(DataTamerParser::BufferSpan& buffer)
{
  return DataTamerParser::Deserialize<uint32_t>(buffer);
}
