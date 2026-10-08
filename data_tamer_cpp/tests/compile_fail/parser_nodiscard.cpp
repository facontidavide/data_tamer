// Compile-fail probe (see data_tamer_compile_fail_test in tests/CMakeLists.txt).
// As written every result is cast to void and the file compiles. Each DT_IGNORE_*
// macro drops one cast, which must then be an error.
#include "data_tamer_parser/data_tamer_parser.hpp"

#include <cstdint>
#include <string>
#include <vector>

void useResults(const std::string& text)
{
  using namespace DataTamerParser;
  const std::vector<uint8_t> body(16);
  const Schema schema;

#ifdef DT_IGNORE_BUILD_SCHEMA_FROM_TEXT
  BuildSchemaFromText(text);
#else
  (void)BuildSchemaFromText(text);
#endif
#ifdef DT_IGNORE_BUILD_SCHEMA_FROM_YAML
  BuildSchemaFromYaml(text);
#else
  (void)BuildSchemaFromYaml(text);
#endif
#ifdef DT_IGNORE_TO_TEXT
  ToText(schema);
#else
  (void)ToText(schema);
#endif
#ifdef DT_IGNORE_GET_BIT
  GetBit(BufferSpan{ body.data(), body.size() }, 0);
#else
  (void)GetBit(BufferSpan{ body.data(), body.size() }, 0);
#endif
#ifdef DT_IGNORE_SPLIT_MCAP_MESSAGE
  SplitMcapMessage(BufferSpan{ body.data(), body.size() });
#else
  (void)SplitMcapMessage(BufferSpan{ body.data(), body.size() });
#endif
}
