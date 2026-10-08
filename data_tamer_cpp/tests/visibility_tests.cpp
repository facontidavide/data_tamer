// The library's inline code compiled into a consumer built with -fvisibility=hidden.
#include "data_tamer/channel.hpp"
#include "data_tamer/sinks/dummy_sink.hpp"
#include "hang_watchdog.hpp"
#include "test_sinks.hpp"

#include <gtest/gtest.h>

#include <vector>
#if defined(DATA_TAMER_HIDDEN_CONSUMER)
#include <dlfcn.h>
#endif

using namespace DataTamer;

// A transaction opened by the library nests the module's LoggedValue::set(): one
// thread-local transaction chain per process, whatever the consumer's visibility.
TEST(Visibility, TransactionsNestAcrossAHiddenVisibilityModule)
{
#if defined(DATA_TAMER_HIDDEN_CONSUMER)
  DataTamerTest::expectFinishes([] {
    void* module = dlopen(DATA_TAMER_HIDDEN_CONSUMER, RTLD_NOW | RTLD_LOCAL);
    ASSERT_NE(module, nullptr) << dlerror();
    using Write = void (*)(LogChannel&, LoggedValue<std::vector<double>>&);
    const char* symbol = "dataTamerHiddenConsumerWrite";
    const auto write = reinterpret_cast<Write>(dlsym(module, symbol));
    ASSERT_NE(write, nullptr) << dlerror();
    auto channel = LogChannel::create("visibility");
    auto value = channel->createLoggedValue<std::vector<double>>("value");
    auto sink = DataTamerTest::manual<DummySink>();
    channel->addDataSink(sink);
    channel->startLogging();
    write(*channel, *value);
    EXPECT_EQ(value->get(), (std::vector<double>{ 1.0, 2.0 }));
    EXPECT_EQ(channel->takeSnapshot(), SnapshotResult::ok);  // the transaction ended
  });
#else
  GTEST_SKIP() << "needs libdata_tamer as a shared library (ROS 2, BUILD_SHARED_LIBS=ON)";
#endif
}
