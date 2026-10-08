![Data Tamer](data_tamer_logo.png)

[![cmake Ubuntu](https://github.com/facontidavide/data_tamer/actions/workflows/cmake_ubuntu.yml/badge.svg)](https://github.com/facontidavide/data_tamer/actions/workflows/cmake_ubuntu.yml)
[![ros2 humble](https://github.com/facontidavide/data_tamer/actions/workflows/ros2-humble.yml/badge.svg)](https://github.com/facontidavide/data_tamer/actions/workflows/ros2-humble.yml)
[![ros2 jazzy](https://github.com/facontidavide/data_tamer/actions/workflows/ros2-jazzy.yml/badge.svg)](https://github.com/facontidavide/data_tamer/actions/workflows/ros2-jazzy.yml)
[![ros2 lyrical](https://github.com/facontidavide/data_tamer/actions/workflows/ros2-lyrical.yml/badge.svg)](https://github.com/facontidavide/data_tamer/actions/workflows/ros2-lyrical.yml)
[![ros2 rolling](https://github.com/facontidavide/data_tamer/actions/workflows/ros2-rolling.yml/badge.svg)](https://github.com/facontidavide/data_tamer/actions/workflows/ros2-rolling.yml)
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

You can easily create your own, specialized sinks.

Use [PlotJuggler](https://github.com/facontidavide/PlotJuggler) to
visualize your logs offline or in real-time.

## Features

- **Serialization schema is created at run-time**: no need to do code generation.
- **Suitable for real-time applications**: very low latency (on the side of the callee);
  `tryTakeSnapshot()` neither blocks nor allocates.
- **Multi-sink architecture**: recorded data can be forwarded to multiple "backends".
- **Very low serialization overhead**, in the order of 1 bit per traced value.
- The user can enable/disable traced variables at run-time.
- **Flight recorder**: `MCAPRingSink` keeps the last seconds in RAM and writes an MCAP file
  only when you ask, for instance after a fault.

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

  // Store the current value of all the registered values.
  // SnapshotResult::ok means that every sink accepted it.
  auto result = channel->takeSnapshot();

  // You can disable (i.e., stop recording) a value like this
  channel->setEnabled(id1, false);
  // or, in the case of a LoggedValue
  logged_real->setEnabled(false);

  // The next snapshot will contain only [value_int], i.e. [id2],
  // since the other two were disabled
  result = channel->takeSnapshot();
}
```

## How to register custom types

Containers such as `std::vector` and `std::array` are supported out of the box.
You can also register a custom type, as shown in the example below.

```cpp
#include "data_tamer/data_tamer.hpp"
#include "data_tamer/channel.hpp"
#include "data_tamer/sinks/mcap_sink.hpp"
#include "data_tamer/custom_types.hpp"

namespace MyNamespace
{
// a custom type
struct Point3D
{
  double x;
  double y;
  double z;
};

// TypeDefinition must be in the same namespace as Point3D.
// List all the member variables that you want to be saved (including their name)
template <typename AddField>
std::string_view TypeDefinition(Point3D& point, AddField& add)
{
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

  MyNamespace::Point3D position = {0.1, -0.2, 0.3};
  channel->registerValue("position", &position);

  // save the data as usual ...
  auto result = channel->takeSnapshot();
}
```

For types whose namespace you can't modify (Eigen, a vendor SDK, ...), specialize
`DataTamer::TypeDefinitionTrait` instead.

More examples, including publishing on ROS 2 and the flight recorder, are in
[data_tamer_cpp/examples](data_tamer_cpp/examples).

# Documentation

- [User manual](docs/USER_MANUAL.md): registration, threads, real-time use, sinks,
  troubleshooting and FAQ.
- [Reference for coding agents](docs/llm_user_manual.md): every rule and corner case.
- [Wire format](docs/wire_format.md): the serialization format.
- [CHANGELOG](data_tamer_cpp/CHANGELOG.rst): what changed in each version.

# Compilation

## Compiling with ROS2

Just use colcon :)

## Compiling with Conan (not ROS2 support)

Note that the ROS2 publisher will **NOT** be built when using this method.

Assuming conan 2.x installed. From the `data_tamer_cpp` directory:

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

The format is specified in [docs/wire_format.md](docs/wire_format.md), and there are two
reference decoders:

- C++: [data_tamer_parser.hpp](data_tamer_cpp/include/data_tamer_parser/data_tamer_parser.hpp),
  a single header without external dependencies that you can copy into your project.
  See how it is used in [mcap_reader](data_tamer_cpp/examples/mcap_reader.cpp).
- Python: [python/data_tamer_parser.py](python/data_tamer_parser.py), standard library only,
  packaged as `data-tamer-parser` (see [python/README.md](python/README.md)).
