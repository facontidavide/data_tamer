// A consumer compiled with -fvisibility=hidden, as packages that set
// CMAKE_CXX_VISIBILITY_PRESET to hidden are; visibility_tests.cpp loads it with dlopen().
#include "data_tamer/channel.hpp"

#include <vector>

// The documented pattern: scopedWrite() (defined in the library) around a non-scalar
// LoggedValue::set() (inlined here), which must nest in the transaction.
extern "C" __attribute__((visibility("default"))) void dataTamerHiddenConsumerWrite(
    DataTamer::LogChannel& channel, DataTamer::LoggedValue<std::vector<double>>& value)
{
  auto tx = channel.scopedWrite();
  value.set({ 1.0, 2.0 });
}
