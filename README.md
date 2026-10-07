![Data Tamer](data_tamer_logo.png)

[![cmake Ubuntu](https://github.com/facontidavide/data_tamer/actions/workflows/cmake_ubuntu.yml/badge.svg)](https://github.com/facontidavide/data_tamer/actions/workflows/cmake_ubuntu.yml)
[![ros2 humble](https://github.com/PickNikRobotics/data_tamer/actions/workflows/ros2-humble.yml/badge.svg)](https://github.com/PickNikRobotics/data_tamer/actions/workflows/ros2-humble.yml)
[![ros2 jazzy](https://github.com/PickNikRobotics/data_tamer/actions/workflows/ros2-jazzy.yml/badge.svg)](https://github.com/PickNikRobotics/data_tamer/actions/workflows/ros2-jazzy.yml)
[![ros2 lyrical](https://github.com/PickNikRobotics/data_tamer/actions/workflows/ros2-lyrical.yml/badge.svg)](https://github.com/PickNikRobotics/data_tamer/actions/workflows/ros2-lyrical.yml)
[![ros2 rolling](https://github.com/PickNikRobotics/data_tamer/actions/workflows/ros2-rolling.yml/badge.svg)](https://github.com/PickNikRobotics/data_tamer/actions/workflows/ros2-rolling.yml)
[![codecov](https://codecov.io/gh/facontidavide/data_tamer/graph/badge.svg?token=D0wtsntWds)](https://codecov.io/gh/facontidavide/data_tamer)

**DataTamer** is a library to log/trace numerical variables over time and
takes periodic "snapshots" of their values, to later visualize them as **timeseries**.

It works great with [PlotJuggler](https://github.com/facontidavide/PlotJuggler),
the timeseries visualization tool (note: you will need PlotJuggler **3.8.2** or later).

**DataTamer** is "fearless data logger" because you can record hundreds or **thousands of variables**:
even 1 million points per second should have a fairly small CPU overhead.

Since all the values are aggregated in a single "snapshot", it is usually meant to
record data in a periodic loop (a very frequent use case, in robotics applications).

Kudos to [pal_statistics](https://github.com/pal-robotics/pal_statistics), for inspiring this project.

## How it works

![architecture](concepts.png)

DataTamer can be used to monitor multiple variables in your applications.

**Channels** are used to take "snapshots" of a subset of variables at a given time.
If you want to record at different frequencies, you can use different channels.

DataTamer will forward the collected data to 1 or multiple **sinks**;
a sink may save the information immediately in a file (currently, we support [MCAP](https://mcap.dev/))
or publish it using an inter-process communication, for instance, a ROS2 publisher.

You can easily create your own, specialized sinks: implement `DataTamer::DataSink`
(two callbacks, `onSchema` and `onSnapshot`, plus optional `onStop` and `onStart` to finish at
shutdown and reopen on restart)
and wrap it with `SinkWorker::create<MySink>()`, which owns the delivery thread and the queues
of the channels attached to it. See `data_tamer/sinks/dummy_sink.hpp` for a small one.

Headers that only name DataTamer types (a `LogChannel&` parameter, a `std::shared_ptr<SinkWorker>`
member, ...) can include `data_tamer/fwd.hpp` instead of the full headers. Like `<iosfwd>`, it
forward-declares `LogChannel`, `ChannelsRegistry`, `SinkWorker`, `DataSink`, `Schema`, `Snapshot`,
`SnapshotRef`, `RegistrationID` and `LoggedValue<T>`, and has no includes.

Use [PlotJuggler](https://github.com/facontidavide/PlotJuggler) to
visualize your logs offline or in real-time.

## Features

- **Serialization schema is created at run-time**: no need to do code generation.
- **Suitable for real-time applications**: very low latency (on the side of the callee).
- **Multi-sink architecture**: recorded data can be forwarded to multiple "backends".
- **Very low serialization overhead**, in the order of 1 bit per traced value.
- The user can enable/disable traced variables at run-time.

## Limitations

- New traced variables can not be added once the first `takeSnapshot()` attempt
  freezes the schema. A previously removed name may be re-registered after that
  point only with its compatible original type, reusing its schema slot.
- Focused on periodic recording. Not the best option for sporadic, asynchronous events.
- If you use `DataTamer::registerValue` you must be careful about the lifetime of the
object: the channel borrows the pointer until `unregister()` has returned. If you prefer a
safer RAII interface, use `DataTamer::createLoggedValue` instead; note that its destructor
unregisters, which waits for a snapshot in progress, so do not drop the last `shared_ptr`
on a real-time thread.
- `registerValue()` returns an opaque `RegistrationID` for `setEnabled()` / `isEnabled()` /
  `unregister()`. After a value is unregistered and registered again under the same name, the
  old id is stale: `setEnabled()` and `unregister()` throw `std::invalid_argument` instead of
  touching the replacement, `isEnabled()` returns false, and `trySetEnabled()` returns false
  without throwing (use it on real-time threads).
- Value names are unique per channel and contain no spaces. Names with empty `/`-separated
  components such as `"/loco//LF/x/"` are accepted, but PlotJuggler shows them as empty path
  elements: build hierarchical names with `DataTamer::JoinNames("loco/", leg, "x")`, which
  collapses the slashes, and check them with `DataTamer::IsCanonicalName()` if you want to
  enforce that (e.g. `assert(DataTamer::IsCanonicalName(name))`).
- `LoggedValue::set()` only stores; a value disabled with `setEnabled(false)` stays disabled
  until `setEnabled(true)`.

## Real-time snapshot contract

- One thread per channel calls `takeSnapshot()`. `prepare()` freezes the schema, pre-allocates
  a pool of 64 snapshots and announces the schema to the sinks; the first `takeSnapshot()` with
  sinks attached calls it for you. Tune beforehand with `setPoolCapacity()`, in slots or in
  time (`setPoolCapacity(200ms, 1ms)` absorbs a 200 ms sink stall at 1 kHz), and
  `setPayloadCapacity()`.
- The pool is the only bound on snapshots in flight. Each sink gets one queue per channel,
  as long as the pool, so a snapshot is refused (`partial`, `rejected`) only by a stopped
  `SinkWorker`. There is no queue size to configure.
- `takeSnapshot()` returns a `SnapshotResult` (`ok`, `partial`, `rejected`, `no_sinks`,
  `pool_exhausted`, ...). It may wait for a writer holding the mutex and grows a slot whose
  payload no longer fits. `tryTakeSnapshot()` is the real-time variant: the library does no
  blocking acquisition (`blocked`) and no allocation (`oversize`) on that path, and it requires
  `prepare()` (`not_prepared`). Custom serializers must follow the same rules there.
- Scalar `LoggedValue::set()` / `get()` are wait-free atomics: each value is captured
  untorn, but two separate writes may land in different snapshots. When the values are
  written by a thread other than the one calling `takeSnapshot()` and several of them must be
  consistent with each other in the recording (a position and its velocity, for instance),
  group the writes in a transaction; the snapshot thread then sees all of them or none:

```cpp
{
  auto tx = channel->scopedWrite();
  logged_real->set(3.2f);
  value_int = 43;  // raw registered values share the same mutex
}
```

- Non-scalar values lock automatically. `getMutablePtr()` / `getConstPtr()` guards join a
  `scopedWrite()` on the same thread instead of deadlocking. Keep transactions and guards
  short: the snapshot thread waits on them.
- Backpressure counters: `poolExhausted()`, `droppedSnapshots(sink)`, `payloadReallocations()`,
  `droppedOversize()`, or all at once with `stats()`.
- Registering, unregistering and changing sinks are safe while logging, but call them outside
  `scopedWrite()` and sink callbacks.

The library is built as C++20; its public headers need only C++17 from consumers.
Details in [CHANGELOG.rst](data_tamer_cpp/CHANGELOG.rst); measurements in
[docs/benchmarks](docs/benchmarks/2026-09-12-main-vs-lockfree-frontend.md).

# Documentation

- [docs/llm_user_manual.md](docs/llm_user_manual.md): how to use the library, written for
  coding agents and useful to humans too (registration rules, sinks, real-time sizing,
  common mistakes).
- [CLAUDE.md](CLAUDE.md): how to build, test and contribute.
- [docs/wire_format.md](docs/wire_format.md): the serialization format.

# Examples

## Basic example

```cpp
#include "data_tamer/data_tamer.hpp"
#include "data_tamer/channel.hpp"
#include "data_tamer/sinks/mcap_sink.hpp"

int main()
{
  // Multiple channels can use this sink. Data will be saved in mylog.mcap
  auto mcap_sink = DataTamer::MCAPSink::create("mylog.mcap");
  // Every 10 minutes the recording continues in a new file (mylog_1.mcap, mylog_2.mcap,
  // ...), so nothing is lost. To bound the disk usage instead, keep only the last 10
  // minutes with setCreateNewFileOnReset(false), or never reset: setMaxTimeBeforeReset(0s).

  // Create a channel and attach a sink. A channel can have multiple sinks
  auto channel = DataTamer::LogChannel::create("my_channel");
  channel->addDataSink(mcap_sink);

  // You can register any arithmetic value. You are responsible for their lifetime!
  double value_real = 3.14;
  int value_int = 42;
  auto id1 = channel->registerValue("value_real", &value_real);
  auto id2 = channel->registerValue("value_int", &value_int);

  // If you prefer to use RAII, use this method instead
  // logged_real will unregister itself when it goes out of scope.
  auto logged_real = channel->createLoggedValue<float>("my_real");

  // Store the current value of all the registered values
  channel->takeSnapshot();

  // You can disable (i.e., stop recording) a value like this
  channel->setEnabled(id1, false);
  // or, in the case of a LoggedValue
  logged_real->setEnabled(false);

  // The next snapshot will contain only [value_int], i.e. [id2],
  // since the other two were disabled
  channel->takeSnapshot();
}
```
Scalar `LoggedValue::set()` and `get()` use relaxed atomics. Each value is
read without tearing, but separate writes can appear in different snapshots.
To capture several updates together, use one logged struct or a transaction:

```cpp
{
  auto tx = channel->scopedWrite();
  logged_real->set(3.2f);
  value_int = 43;  // a raw registered value uses the same write mutex
}
channel->takeSnapshot();
```

Non-scalar `set()`/`get()` lock automatically and can be called inside
`scopedWrite()`. Keep transactions short and take snapshots after releasing
them. Non-scalar pointer proxies also hold the write mutex; release them before
starting a transaction or taking a snapshot.

## Registry defaults and shutdown

`ChannelsRegistry` creates channels by name, applies default settings to each new channel
and attaches the default sinks to every channel, including the ones that exist already.
`SinkWorker::stop()` delivers what is queued and then lets the sink finish itself
(`DataSink::onStop()`): `MCAPSink` closes its file, `MCAPRingSink` writes a pending dump,
`ROS2PublisherSink` publishes its partial batch. `stopAll()` does that for every sink of the
registry, so shutdown needs no ordering rules. `SinkWorker::start()` resumes a stopped sink
(`DataSink::onStart()`: `MCAPSink` continues in its next numbered file).

```cpp
using namespace std::chrono_literals;
auto& registry = DataTamer::ChannelsRegistry::Global();
DataTamer::ChannelDefaults defaults;
defaults.pool_stall_tolerance = 200ms;  // pool sized to absorb a 200 ms sink stall
defaults.pool_snapshot_period = 1ms;    // at 1 kHz: 200 slots per channel
registry.setChannelDefaults(defaults);  // channels created from now on
registry.addDefaultSink(DataTamer::MCAPSink::create("run.mcap"));
auto channel = registry.getChannel("controller");
// ... register values, prepare(), take snapshots ...
registry.stopAll();  // every sink stopped once; run.mcap is complete
```

## How to register custom types

Containers such as `std::vector` and `std::array` are supported out of the box.
You can also register a custom type, as shown in the example below.

```cpp
#include "data_tamer/data_tamer.hpp"
#include "data_tamer/channel.hpp"
#include "data_tamer/sinks/mcap_sink.hpp"
#include "data_tamer/custom_types.hpp"

// This is your custom type
namespace MyNamespace
{
struct Point3D
{
  double x;
  double y;
  double z;
};
} // end namespace MyNamespace

// You must implement the function TypeDefinition in the same namespace as Point3D
// (or specialize DataTamer::TypeDefinitionTrait, see below)
namespace MyNamespace
{
template <typename AddField>
std::string_view TypeDefinition(Point3D& point, AddField& add) {
  add("x", &point.x);
  add("y", &point.y);
  add("z", &point.z);
  return "Point3D";
}
} // end namespace MyNamespace

int main()
{
  auto channel = DataTamer::LogChannel::create("my_channel");
  channel->addDataSink(DataTamer::MCAPSink::create("mylog.mcap"));

  // Array/vectors are supported natively
  std::vector<double> values = {1, 2, 3, 4};
  channel->registerValue("values", &values);

  // Requires the implementation of DataTamer::TypeDefinition<Point3D>
  Point3D position = {0.1, -0.2, 0.3};
  channel->registerValue("position", &position);

  // save the data as usual ...
  channel->takeSnapshot();
}
```

### Types you can't (or don't want to) modify

`TypeDefinition` is found by argument-dependent lookup, so it must live in the
namespace of the type. For third-party types (Eigen, a vendor SDK, ...) you can
instead specialize `DataTamer::TypeDefinitionTrait` and leave their namespace
alone:

```cpp
#include "data_tamer/custom_types.hpp"
#include <third_party/geometry.hpp>  // defines third_party::Point

template <>
struct DataTamer::TypeDefinitionTrait<third_party::Point>
{
  template <typename AddField>
  static std::string_view define(third_party::Point& p, AddField& add)
  {
    add("x", &p.x);
    add("y", &p.y);
    return "Point";
  }
};
```

Partial specializations work too; the second, defaulted template parameter
accepts `std::enable_if_t<...>` / `std::void_t<...>`. For class templates the
name can be built at runtime with an optional `static std::string name()`: it
is evaluated once per type (per shared library, like any function-local
static), and `define()` may then return `void`. A `define()` that returns a
`std::string` is cached the same way; a `std::string_view` or `const char*`
must point to storage that outlives the program, such as a string literal.

```cpp
template <typename T, int N>
struct DataTamer::TypeDefinitionTrait<Eigen::Matrix<T, N, 1>>
{
  static std::string name() { return "Vector" + std::to_string(N); }

  template <typename AddField>
  static void define(Eigen::Matrix<T, N, 1>& v, AddField& add)
  {
    static_assert(N <= 4);
    static const char* names[] = { "x", "y", "z", "w" };
    for(int i = 0; i < N; i++)
    {
      add(names[i], &v[i]);
    }
  }
};
```

The specialization must be visible wherever the type is registered. If a type
has both a `TypeDefinitionTrait` specialization and a `TypeDefinition` overload,
the trait is used. A specialization whose `define()` can not be called as
`define(T&, AddField&)` with a generic `AddField` is a compile error rather
than being ignored.

## Publishing on ROS 2

`ROS2PublisherSink` publishes the schemas (latched) on `<prefix>/schemas` and, by
default, one `data_tamer_msgs/Snapshot` per sample on `<prefix>/data`. To publish
fewer messages, aggregate the snapshots into `data_tamer_msgs/SnapshotBatch`
messages on `<prefix>/data_batch`:

```cpp
#include "data_tamer/sinks/ros2_publisher_sink.hpp"

DataTamer::ROS2PublisherOptions options;
options.aggregate = true;
options.max_batch_size = 100;                             // snapshots per message...
options.max_batch_delay = std::chrono::milliseconds(100);  // ...or this old, checked per snapshot
options.embed_schemas = true;  // each batch carries its schemas: self-contained
options.schema_format = DataTamer::SchemaFormat::Yaml;  // optional: shorter schemas, docs/wire_format.md 2.1

auto sink = DataTamer::ROS2PublisherSink::create(node, "/robot", options);
channel->addDataSink(sink);
// a partial batch is published by the next snapshot, by flush() or by sink->stop():
sink->as<DataTamer::ROS2PublisherSink>().flush();
```

Each message on `<prefix>/schemas` is the complete schema catalog, published as soon as a
channel is prepared (reliable, transient-local, depth 1: late subscribers get the latest
catalog). Call `channel->prepare()` before the control loop: otherwise the first
`takeSnapshot()` prepares the channel and does that DDS write. The data topic uses
`options.data_qos`, by default `rclcpp::QoS(rclcpp::KeepLast(100)).reliable()`: memory is
bounded, and a slow or stalled subscriber loses the oldest messages instead of making the
publishing process queue them without bound. Pick another depth, or
`rclcpp::QoS(rclcpp::KeepLast(10)).best_effort()`, to suit the rate (with aggregation each
message is a batch), or `rclcpp::QoS(rclcpp::KeepAll()).reliable()` for a lossless stream at
the price of unbounded memory. A best-effort publisher does not match
reliable subscribers, so they must use best effort too (a best-effort subscriber, as in
`ros2_subscriber.py`, matches either).

[ros2_publisher](data_tamer_cpp/examples/ros2_publisher.cpp) (`--aggregate`, `--yaml`) and
[python/ros2_subscriber.py](python/ros2_subscriber.py) show both ends.

## Flight recorder: dump the last seconds on demand

`MCAPRingSink` keeps the last `window` of every attached channel in a preallocated RAM ring
and writes an MCAP file only when you call `requestDump()`, for instance on a protective stop
or a fault. See [T04_flight_recorder.cpp](data_tamer_cpp/examples/T04_flight_recorder.cpp).

```cpp
#include "data_tamer/sinks/mcap_ring_sink.hpp"

DataTamer::MCAPRingOptions options;
options.filepath = "fault.mcap";          // fault_1.mcap, fault_2.mcap, ... never overwritten
options.window = std::chrono::seconds(5);  // history before the trigger
options.capacity_bytes = 64 << 20;         // RAM ring; the sink uses about twice this
auto worker = DataTamer::MCAPRingSink::create(options);
channel->addDataSink(worker);
auto& recorder = worker->as<DataTamer::MCAPRingSink>();

// From any thread, real-time ones included: one atomic compare-exchange.
recorder.requestDump(std::chrono::seconds(2));  // also keep 2 s after the event

// At shutdown, stop() delivers what is queued and writes a dump requested just before it.
worker->stop();
```

- The trigger is the first snapshot delivered after the request, and all times are snapshot
  timestamps: with trigger time `T` the file holds `[T - window, T + post_trigger]`, so a dump
  behaves the same in simulation, replay and on hardware. It is complete at the first snapshot
  stamped after `T + post_trigger`; channels are queued separately, so a snapshot of another
  channel delivered later than that one is not included even if stamped earlier.
- `onSnapshot()` copies the data into the ring and releases the pool slot at once; it allocates
  nothing after the ring is allocated. When the ring is full, the oldest snapshots are evicted
  even if younger than `window`; a dump that lost data that way reports `truncated`.
- A writer thread writes the file; the sink worker never waits for disk. While a request is
  active (until its dump is handed to the writer) further requests return `false` and are
  ignored. `setDumpCallback()` reports each file (including write errors such as a full disk),
  `stats()` the counters. Dump numbers whose file exists already are skipped.
- `stats().oldest_timestamp` and `newest_timestamp` give the interval the ring holds now:
  if it stays shorter than `window` while `evicted_by_capacity` grows, the ring is too small.
- `flushPendingDump()` writes the active request at once and waits for the file. It works
  with the worker running; at shutdown, call it after `stop()` so that every queued snapshot
  is in the ring first.

# Compilation

## Compiling with ROS2

Just use colcon :)

## Compiling with Conan (not ROS2 support)

Note that the ROS2 publisher will **NOT** be built when using this method.

Assuming conan 2.x installed. From the source directory.

**Release**:

```
conan install . -s compiler.cppstd=gnu17 --build=missing -s build_type=Release
cmake -S . -B build/Release -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE="build/Release/generators/conan_toolchain.cmake"
cmake --build build/Release --parallel
```

**Debug**:

```
conan install . -s compiler.cppstd=gnu17 --build=missing -s build_type=Debug
cmake -S . -B build/Debug -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_TOOLCHAIN_FILE="build/Debug/generators/conan_toolchain.cmake"
cmake --build build/Debug --parallel
```

# How to deserialize data recorded with DataTamer

The wire format is specified in [docs/wire_format.md](docs/wire_format.md), with golden
byte vectors under `docs/wire_format/vectors/` that the test suite checks on every run.
Two reference decoders implement it:

- C++, a single header without external dependencies that you can copy into your project:
  [data_tamer_parser.hpp](data_tamer_cpp/include/data_tamer_parser/data_tamer_parser.hpp),
  used in [mcap_reader](data_tamer_cpp/examples/mcap_reader.cpp).
- Python, standard library only: [python/data_tamer_parser.py](python/data_tamer_parser.py),
  packaged as `data-tamer-parser` (`pip install ./python`, see [python/README.md](python/README.md)).
  `iter_mcap("log.mcap")` reads a whole MCAP file (with `pip install "./python[mcap]"`) and
  `schema.field_names()` lists the flattened names of a schema without decoding a message.

Both read either schema rendering (the line format and YAML) and include helpers for the
ROS 2 messages that need no ROS dependency, since they only read message fields:

```cpp
DataTamerParser::SchemaRegistry registry;  // registry.addSchemas(schemas_msg) for /schemas
DataTamerParser::ForEachSnapshotInBatch(registry, batch_msg,
    [](const DataTamerParser::Schema& schema, const DataTamerParser::SnapshotView& snapshot) {
      DataTamerParser::ParseSnapshot(schema, snapshot, [&](const std::string& name,
                                                          const DataTamerParser::VarNumber& value) {
        // ...
      });
    });
// a single data_tamer_msgs/Snapshot: ParseSnapshot(schema, ToSnapshotView(msg), ...)
```

```python
registry = data_tamer_parser.SchemaRegistry()   # registry.add_schemas(schemas_msg) for /schemas
for schema, timestamp_nsec, values in data_tamer_parser.iter_snapshot_batch(registry, batch_msg):
    print(schema.channel_name, timestamp_nsec, values)   # values: {"pose/position/x": 1.0, ...}
```
