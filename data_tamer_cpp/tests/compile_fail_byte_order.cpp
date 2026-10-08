// The parser reads numbers with a plain memcpy, so it needs the little endian host the
// wire format assumes. Compiled as for a big endian host (see CMakeLists.txt, which
// redefines __BYTE_ORDER__ for the rejected case only), it must refuse to build.
#include "data_tamer_parser/data_tamer_parser.hpp"

uint32_t countOf(DataTamerParser::BufferSpan& buffer)
{
  return DataTamerParser::Deserialize<uint32_t>(buffer);
}
