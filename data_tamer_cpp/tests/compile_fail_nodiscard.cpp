// Dropping the result of these functions is a mistake: it must not compile quietly.
// Each rejected build (see CMakeLists.txt) defines one IGNORE_* macro; the other
// calls, and all of them in the control build, cast their result to void.
#include "data_tamer_parser/data_tamer_parser.hpp"

#include <cstdint>
#include <string>
#include <vector>

void useResults(const std::string& text)
{
  using namespace DataTamerParser;
  const std::vector<uint8_t> body(16);
  const Schema schema;

#ifdef IGNORE_BUILD_SCHEMA_FROM_TEXT
  BuildSchemaFromText(text);
#else
  (void)BuildSchemaFromText(text);
#endif
#ifdef IGNORE_BUILD_SCHEMA_FROM_YAML
  BuildSchemaFromYaml(text);
#else
  (void)BuildSchemaFromYaml(text);
#endif
#ifdef IGNORE_TO_TEXT
  ToText(schema);
#else
  (void)ToText(schema);
#endif
#ifdef IGNORE_GET_BIT
  GetBit(BufferSpan{ body.data(), body.size() }, 0);
#else
  (void)GetBit(BufferSpan{ body.data(), body.size() }, 0);
#endif
#ifdef IGNORE_SPLIT_MCAP_MESSAGE
  SplitMcapMessage(BufferSpan{ body.data(), body.size() });
#else
  (void)SplitMcapMessage(BufferSpan{ body.data(), body.size() });
#endif
}
