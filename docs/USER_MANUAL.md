# DataTamer user manual

This manual starts where the [README](../README.md) stops. The chapters explain how to use
each part of the library. [Troubleshooting](#troubleshooting) starts from a symptom, and
the [FAQ](#faq) collects short answers that fit nowhere else. Code written for 1.x needs
the changes listed in [MIGRATION.md](MIGRATION.md).

- [Concepts](#concepts)
- [Registering values](#registering-values)
- [Writing values from other threads](#writing-values-from-other-threads)
- [Real-time use](#real-time-use)
- [Sinks](#sinks)
- [Reading the data](#reading-the-data)
- [Building and linking](#building-and-linking)
- [Troubleshooting](#troubleshooting)
- [FAQ](#faq)

Follow the [progressive tutorials](../data_tamer_cpp/examples/README.md) for runnable
examples of a small robot arm, from a first MCAP recording to real-time logging and threads.

## Concepts

- A **channel** (`LogChannel`) owns a set of registered values and takes snapshots of them.
  Use one channel per rate: a 1 kHz controller and a 10 Hz planner get one channel each.
- A **snapshot** is the value of every enabled variable of a channel at one time stamp.
- The **schema** describes the values of a channel. It freezes when the channel
  starts logging, and the sinks receive it once.
- A **sink** receives schemas and snapshots and does something with them: `MCAPSink`
  writes a file, `MCAPRingSink` keeps the last seconds in RAM, `ROS2PublisherSink`
  publishes ROS 2 messages. Each sink runs in a `SinkWorker`, which owns its thread.
- The **pool** is a fixed set of preallocated snapshots per channel, shared by its sinks.
  A snapshot occupies a slot until every sink is done with it.
- `ChannelsRegistry` creates channels by name and can attach default sinks to all of them.

## Registering values

### What you can register

`registerValue(name, &variable)` accepts integers, `float`, `double`, `bool`, `char`,
enums, `std::atomic` of those, `std::vector`, `std::array` and your own types (see
[custom types](#custom-types)). It returns a `RegistrationID`. Integers are recorded by
size and signedness, so `long long` works; `long double` is not supported, use `double`.

Text works too: a `std::string` (also a `std::pmr::string`) or a `std::string_view`,
registered, as a member of a custom type, or in a `std::vector` or `std::array`. The
schema calls it `string`. It's recorded as its length and its bytes, read at every
snapshot, so it can change length from one snapshot to the next. The decoders return it
as one text value: a `str` in Python, a string in TypeScript and Foxglove, a
`std::string_view` in C++ when you pass a string callback (see
[Reading the data](#reading-the-data)). Things to know:

- `tryTakeSnapshot()` doesn't grow its slot: a text longer than the slot drops the
  whole snapshot as `oversize`, numbers included. Call `setPayloadCapacity()` with room
  for the longest text.
- Assigning a `std::string` allocates once it outgrows its capacity. In a real-time loop,
  `reserve()` it once beforehand, or log a `std::string_view` of text that stays alive,
  such as a string literal (`state = "IDLE";`): the snapshot reads the characters it
  points to.
- Write a text that another thread reads inside `scopedWrite()`, like a vector.
- Write UTF-8: the decoders read it as UTF-8 and replace invalid bytes with U+FFFD.

A `std::vector<char>` or `std::array<char, N>` is not text: it stays one value per
character. A `const char*` doesn't register: use a `std::string_view`.

### When you can register

Register everything before the first snapshot. The schema freezes at `channel->startLogging()`,
or at the first `takeSnapshot()` or `tryTakeSnapshot()` that finds a sink attached, and registering a new name
after that throws `std::runtime_error`. A name you unregistered can be registered again
after the freeze, but only with its original type, because it reuses its slot.

### Lifetime of registered variables

Choose by who writes the value:

| Written by | Register it with |
|---|---|
| Only the thread that takes the snapshots | `registerValue("x", &x)`: borrowed, so `x` must outlive the registration or be unregistered first. |
| Another thread | `createLoggedValue<T>("x")`: the channel owns it. A scalar is atomic; a struct or vector goes through `getMutablePtr()`. Raw `registerValue()` variables are written inside `scopedWrite()`, which also keeps several values consistent. |

See [Writing values from other threads](#writing-values-from-other-threads).
`registerValue("x", &x)` borrows the pointer and reads it at every snapshot until
`unregister()` returns or the channel is destroyed. A dangling pointer is not detected, so:

- declare registered variables before the channel, locals and class members alike;
- register a `std::vector` itself, not one of its elements, because growing the vector
  moves them;
- don't register a member of an object that is later moved or copied.

If a value has no natural owner, let the channel create it (a `LoggedValue`):

```cpp
auto speed = channel->createLoggedValue<double>("speed");  // unregisters in its destructor
speed->set(0.5);
```

The `LoggedValue` destructor waits for a snapshot in progress, so don't drop the last
`shared_ptr` on a real-time thread.

### Names

Names are unique per channel, not empty, and contain no whitespace or control character.
The names of custom types and of their fields follow the same rule. A channel name can
contain spaces, but it can't be empty or contain a control character.
A registration that throws changes nothing, so you can catch the exception and carry on.
Use `/` to build a hierarchy, which
PlotJuggler shows as a tree. `DataTamer2::JoinNames()` drops empty components, so you don't
end up with `loco//LF/x`:

```cpp
#include "data_tamer_v2/names.hpp"

const auto name = DataTamer2::JoinNames("loco/", leg_name, "x");
assert(DataTamer2::IsCanonicalName(name));  // optional check
channel->registerValue(name, &x);
```

### Enabling and disabling

`setEnabled(id, false)` stops recording a value until `setEnabled(id, true)`. A disabled
value costs one bit per snapshot, and `LoggedValue::set()` doesn't re-enable it. Turning
values on and off is much cheaper than registering and unregistering them, and it is
allowed after the schema froze.

`setEnabled()` throws `std::invalid_argument` for a stale id, one whose name was
unregistered and registered again. `trySetEnabled()` returns false instead: use it on
real-time threads.

### Custom types

The [README example](../README.md#how-to-register-custom-types) and
[T02_custom_types.cpp](../data_tamer_cpp/examples/T02_custom_types.cpp) put a `TypeDefinition()`
function in the namespace of the type. For types whose namespace you don't own, such as
Eigen or a vendor SDK, specialize `DataTamer2::TypeDefinitionTrait` instead:

```cpp
#include "data_tamer_v2/custom_types.hpp"

template <>
struct DataTamer2::TypeDefinitionTrait<Eigen::Vector3d>
{
  template <typename AddField>
  static std::string_view define(Eigen::Vector3d& v, AddField& add)
  {
    add("x", &v.x());
    add("y", &v.y());
    add("z", &v.z());
    return "Vector3d";
  }
};
```

The specialization must be visible wherever the type is registered. If a type has both a
trait and a `TypeDefinition()`, the trait wins. Give each type its own name: two types
with one name in a channel make `registerValue()` throw, and so does a type named like
the `typeName()` of a `registerCustomValue()` serializer of the channel (and the
reverse).

## Writing values from other threads

The thread that takes the snapshots can write its registered variables directly. Other
threads follow these rules:

- a scalar `LoggedValue` (a number, `bool` or enum) is atomic, and `set()` is safe from
  any thread;
- a non-scalar `LoggedValue` (a struct, a vector) locks the channel's write mutex in
  `set()` and `get()`;
- a variable registered with `registerValue()` is written only inside a transaction.

Each value is captured whole, but two separate writes can land in different snapshots.
To keep several values consistent with each other, write them in one transaction:

```cpp
{
  auto tx = channel->scopedWrite();
  position->set(p);     // LoggedValue
  velocity->set(v);     // LoggedValue
  mode = Mode::Moving;  // a value registered with registerValue()
}
```

The snapshot thread sees all of the writes or none. Keep transactions short, because the
snapshot thread waits for them.

## Real-time use

`tryTakeSnapshot()` exists to be lock-free: once logging started, it never blocks on a
lock and never allocates, so a real-time loop can call it every cycle. Do the work that
allocates before the loop, and call `tryTakeSnapshot()` inside it:

```cpp
// before the loop, on a normal thread
channel->startLogging();  // freeze the schema, allocate, announce to the sinks

// inside the loop
if(!DataTamer2::accepted(channel->tryTakeSnapshot()))  // ok or partial
{
  ++snapshot_failures;  // count it, report it from another thread
}
```

Without a sink, `tryTakeSnapshot()` returns `no_sinks`, in real time. Without
`startLogging()`, the first `tryTakeSnapshot()` with a sink attached calls it, as
`takeSnapshot()` does: that call allocates and runs the sinks' `onSchema()`, so it is not
real-time (RealtimeSanitizer reports it), and if it fails it returns `start_failed` and
the next call tries again. Call `startLogging()` before the loop when the first cycle must
be real-time too.

Where `takeSnapshot()` would wait for a writer or grow a slot, `tryTakeSnapshot()` gives up
and returns `blocked` or `oversize`. It reports every failure as a `SnapshotResult` and
doesn't throw: an exception from a custom serializer inside it becomes `serialize_failed`
(the throw itself allocates: fix the serializer). Several threads may take snapshots of
one channel: they take turns on its write mutex, and `tryTakeSnapshot()` returns
`blocked` while another snapshot holds it.
`tryTakeSnapshot()` doesn't allocate when data_tamer is loaded with `dlopen()` either, in
a plugin for example.

With Clang 20 or later the compiler and RealtimeSanitizer can check your loop. The
real-time calls of data_tamer are marked `[[clang::nonblocking]]`; mark your cycle the
same way:

```cpp
void controlCycle() noexcept [[clang::nonblocking]]
{
  velocity->set(v);
  (void)channel->tryTakeSnapshot();
}
```

- `-Wfunction-effects` reports, at compile time, a call in it that is not nonblocking:
  `takeSnapshot()`, a registration, a non-scalar `LoggedValue::set()`.
- `-fsanitize=realtime` (RTSan) reports, at run time, every allocation, lock and system
  call made inside it, data_tamer's included, also with a prebuilt data_tamer, and
  `takeSnapshot()`. On Linux (ELF) only, the few that data_tamer makes on purpose are
  not reported: releasing the write mutex to a writer that waits for it, and waking a
  sleeping sink worker, or a control operation or `stop()` that waits for the
  snapshot. Elsewhere (macOS) RTSan reports the release of the write mutex in the
  first `tryTakeSnapshot()`. A custom serializer that allocates is always reported.

The pool is sized for you: as many slots as fit in 8 MiB, at least 64 and at most 1024.
With a payload under 8 KB (1000 doubles) that is 1024 slots, one second at 1 kHz, longer
than a disk flush (up to 200 ms). If a sink stalls longer, new snapshots are dropped with
`pool_exhausted`, and `setPoolCapacity(stall, period)` sizes the pool for the stall you
want to absorb. `setPayloadCapacity(bytes)` reserves room for vectors that grow at run
time. Both must be called before `startLogging()`.

## Sinks

A sink is created with its `create()` function, which returns a
`std::shared_ptr<SinkWorkerOf<SinkType>>`: a `SinkWorker` that owns the sink. Attach it to
one or more channels with `addDataSink()`, and reach the sink's own functions with
`worker->sink()`. Code that holds only a `std::shared_ptr<SinkWorker>` uses
`worker->as<SinkType>()`, which throws `std::bad_cast` for the wrong type.

### Recording to MCAP files

```cpp
#include "data_tamer_v2/sinks/mcap_sink.hpp"

auto worker = DataTamer2::MCAPSink::create("run.mcap");
channel->addDataSink(worker);
```

By default `MCAPSink` starts a new numbered file every 10 minutes (`run.mcap`,
`run_1.mcap`, `run_2.mcap`, ...), so no file grows forever and nothing is lost. To change
it:

```cpp
auto& sink = worker->sink();
sink.setMaxTimeBeforeReset(std::chrono::seconds(0));  // one file, never reset
// or: sink.setCreateNewFileOnReset(false);           // keep only the last 10 minutes
```

`setCreateNewFileOnReset(false)` overwrites the file at every reset, so use it only for a
recording you don't keep. If the next file cannot be opened at a reset, the failure is
counted once in `worker->errors()` and the current file keeps recording. A numbered name
that exists already is skipped, but the file at the path you pass to `create()` is
overwritten. `worker->start()` after `stop()` continues in the next numbered file.

A failed write (full disk, I/O error) throws from the sink and shows in
`worker->errors()` and `lastError()`. Closing a file runs `fsync()`, so `stop()` returns
once the file is on disk. On Linux both MCAP sinks keep only about 24 MiB of each file
in the page cache: every 8 MiB they wait for the previous 8 MiB to reach the disk and
drop it from the cache. A large file then doesn't leave gigabytes of dirty pages for the
kernel to flush at once, which stalls other processes on the same disk. The bound
doesn't hold on overlayfs or tmpfs.

### Flight recorder

`MCAPRingSink` keeps the last `window` of every attached channel in RAM and writes an MCAP
file only when you call `requestDump()`, for instance after a fault:

```cpp
#include "data_tamer_v2/sinks/mcap_ring_sink.hpp"

DataTamer2::MCAPRingOptions options;
options.filepath = "fault.mcap";            // fault_1.mcap, fault_2.mcap, ...
options.window = std::chrono::seconds(5);   // history before the trigger
options.capacity_bytes = size_t(64) << 20;  // the sink uses about twice this
auto recorder = DataTamer2::MCAPRingSink::create(options);
channel->addDataSink(recorder);

// from any thread, real-time ones included: keep also 2 s after the trigger
(void)recorder->sink().requestDump(std::chrono::seconds(2));
```

The dump covers `[trigger - window, trigger + post_trigger]` in snapshot time, so it
behaves the same in simulation, in a replay and on the robot. `requestDump()` returns false
while an earlier dump is still in progress (the result is `[[nodiscard]]`). `setDumpCallback()` reports every file written
or failed (the dump is written as described in
[Recording to MCAP files](#recording-to-mcap-files), with `fsync()` and the bounded page
cache). [T06_flight_recorder.cpp](../data_tamer_cpp/examples/T06_flight_recorder.cpp)
is a complete example.

### Publishing on ROS 2

`ROS2PublisherSink` publishes the schemas on `<prefix>/schemas` and one
`data_tamer_msgs/Snapshot` per sample on `<prefix>/data`. To publish fewer messages,
aggregate the samples into batches on `<prefix>/data_batch`:

```cpp
#include "data_tamer_v2/sinks/ros2_publisher_sink.hpp"

DataTamer2::ROS2PublisherOptions options;
options.aggregate = true;
options.max_batch_size = 100;                              // snapshots per message
options.max_batch_delay = std::chrono::milliseconds(100);  // or this old
auto worker = DataTamer2::ROS2PublisherSink::create(node, "/robot", options);
channel->addDataSink(worker);
```

`startLogging()` publishes the schemas, so call it before the control loop. The data topic keeps the last 100 messages (`options.data_qos`): a slow
subscriber loses old messages instead of growing the publisher's memory.
[ros2_publisher.cpp](../data_tamer_cpp/examples/ros2_publisher.cpp) and
[ros2_subscriber.py](../python/ros2_subscriber.py) show both ends.

### Writing your own sink

Derive from `DataTamer2::DataSink`, implement `onSchema()` and `onSnapshot()`, and wrap it in
a `SinkWorker`, which owns the thread that calls them. `addDataSink()` takes the worker,
not the sink: `channel->addDataSink(std::make_shared<CountingSink>())` fails to compile with
a message that says so.

```cpp
class CountingSink : public DataTamer2::DataSink
{
protected:
  void onSchema(const DataTamer2::Schema& schema) override { /* once per channel */ }
  void onSnapshot(const DataTamer2::SnapshotRef& snapshot) override { ++count_; }

private:
  size_t count_ = 0;
};

auto worker = DataTamer2::SinkWorker::create<CountingSink>();
channel->addDataSink(worker);
```

To decode a snapshot there, pass `ToSnapshotView(*snapshot)` (`data_tamer_v2/decode.hpp`)
to `DataTamerParser::ParseSnapshot()`, as
[T07_custom_sink.cpp](../data_tamer_cpp/examples/T07_custom_sink.cpp) does.

Keep `onSnapshot()` short: while it holds a snapshot, that slot of the channel's pool stays
busy for every sink. For slow work, copy the snapshot (`DataTamer2::Snapshot copy =
*snapshot;`) and hand the copy to your own thread. Override `onStop()` and `onStart()` if
the sink has files to close and reopen.

Keep control calls out of the callbacks: a call that would wait for the sink's own worker
throws `std::logic_error` there instead of hanging (the list is in the `DataSink`
documentation, `data_sink.hpp`).

### Shutdown

`stop()` ends a recording: it delivers what is queued, then lets the sink finish:
`MCAPSink` closes its file, `MCAPRingSink` writes a pending dump, `ROS2PublisherSink`
publishes its partial batch.

```cpp
worker->stop();
// or, for every sink of a registry:
DataTamer2::ChannelsRegistry::Global().stopAll();
// or, at the end of main(): stopAll(), then drop the channels that borrow your variables
DataTamer2::ChannelsRegistry::Global().shutdown();
```

Destroying the last reference to a running worker stops it too, so the files are complete
after a normal exit. Call `stop()` where the recording must be complete at a known point,
or before an exit that skips destructors. The global registry outlives `main()`, so its
channels would keep borrowing the locals of `main()`: end it with `shutdown()`.

### Registry defaults

`ChannelsRegistry` creates channels by name, applies default settings to each new channel
and attaches the default sinks to every channel, existing ones included:

```cpp
using namespace std::chrono_literals;

auto& registry = DataTamer2::ChannelsRegistry::Global();
DataTamer2::ChannelDefaults defaults;
defaults.pool_stall_tolerance = 2s;  // with the next line: 2000 slots per channel
defaults.pool_snapshot_period = 1ms;
registry.setChannelDefaults(defaults);  // for channels created from now on
registry.addDefaultSink(DataTamer2::MCAPSink::create("run.mcap"));
auto channel = registry.getChannel("controller");
```

## Reading the data

[PlotJuggler](https://github.com/facontidavide/PlotJuggler) 4 or later opens the MCAP
files and subscribes to the ROS 2 topics.

[Foxglove with the extension](../data_tamer_foxglove/extension/README.md) reads ROS 2 topics and rosbag2 recordings.
To plot, use one Snapshot per message (the default, `aggregate = false`); batches are inspectable.
Direct `MCAPSink`/`MCAPRingSink` files are **not readable in Foxglove**: use PlotJuggler 4 or Python.

In Python, install `data-tamer-parser` with its MCAP extra:

```bash
pip install "data-tamer-parser[mcap]"
```

```python
import data_tamer_parser as dt

for timestamp_nsec, channel, values in dt.iter_mcap("run.mcap"):
    print(channel, timestamp_nsec, values["loco/LF/x"])
```

Disabled values are absent from `values`. `iter_mcap()` reads one file, so read numbered
files one after the other. Both decoders also read the files of data_tamer 1.x; files of
releases before 0.7.0 (2023) are not readable. To build columns, for pandas for example,
`dt.parse_snapshot_values()` returns the names and the values of a snapshot as two
tuples instead of a dict, and is faster still:
[python/README.md](../python/README.md) shows how.

In C++, `data_tamer_v2/mcap_reader.hpp` does the same, with the library linked:

```cpp
#include "data_tamer_v2/mcap_reader.hpp"

DataTamer2::ForEachValue("run.mcap", [](const std::string& channel, uint64_t timestamp_nsec,
                                        const std::string& name,
                                        const DataTamerParser::VarNumber& value) {
  // value is a std::variant of the basic types
});
```

Text doesn't come to that callback. To get it, pass a second callback, which receives
`(channel, timestamp_nsec, name, std::string_view text)` for each text value;
`DataTamerParser::ParseSnapshot()` takes the same extra callback, without the channel
and timestamp. The text is the bytes as written, not checked to be UTF-8. Python and
TypeScript decode it as UTF-8, with invalid bytes replaced by U+FFFD.

Values come in log time order, and any error (a file that can't be read, a corrupt
message) throws `std::runtime_error`, whose message names the file and, where there is
one, the channel.
The recording of a process that crashed is read up to its last complete chunk, in the
order it was written.
A third argument, a vector of channel names, restricts the channels read;
`ForEachSnapshot()` hands you each snapshot undecoded instead.
[T08_mcap_reader.cpp](../data_tamer_cpp/examples/T08_mcap_reader.cpp) uses it.

Without the library, copy the single header
[data_tamer_parser.hpp](../data_tamer_cpp/include/data_tamer_v2/data_tamer_parser.hpp)
into your project: it has no dependencies. The
format is specified in [wire_format.md](wire_format.md).

## Building and linking

The library is built as C++20, and its public headers need only C++17. Link it with

```cmake
find_package(data_tamer 2 REQUIRED)
target_link_libraries(my_app PRIVATE data_tamer::data_tamer)
```

after a CMake install, a conan install or, under ROS 2, a colcon build. The ROS 2
package is `data_tamer_v2` (`<depend>data_tamer_v2</depend>`; `find_package(data_tamer_v2)`
with `data_tamer_v2::data_tamer` works too). It installs side by side with
`data_tamer_cpp`, the 1.x package. With
`add_subdirectory()` or FetchContent, link `data_tamer::data_tamer` directly: as a
subproject the library builds without ROS 2, tests or examples, and installs nothing
unless you set `DATA_TAMER_INSTALL=ON`. The README lists the build options.

`data_tamer_v2/sinks/mcap_encoding.hpp` includes the MCAP headers, which come with the
target: MCAP 2.1.3, downloaded when the library is built and installed in
`include/data_tamer_v2_mcap`,
`mcap_vendor` under ROS 2, or the `mcap` package with conan. Outside ROS 2 the MCAP
implementation is part of libdata_tamer, so don't define `MCAP_IMPLEMENTATION` and don't
link another MCAP library. A static install also needs zstd and lz4 where you build; its
CMake package finds them.

The shared library has the SONAME `libdata_tamer.so.2` (`libdata_tamer_v2.so.2` under
ROS 2), so a program built against any 2.x release runs with any later 2.x library, and
`find_package(data_tamer 2 REQUIRED)` accepts any 2.x. Defaults that live in headers
change only when you rebuild. Under ROS 2, one process can also load data_tamer 1.x and
2.x, e.g. two plugins built against different majors: the library's namespace is
`DataTamer2`, and `SerializeMe` and `DataTamerParser` are in an inline namespace `v2`, so
no 2.x symbol clashes with 1.x.

A header that only names DataTamer types, such as a `LogChannel&` parameter or a
`std::shared_ptr<SinkWorker>` member, can include `data_tamer_v2/fwd.hpp` instead of the full
headers. It forward-declares the types and includes nothing.

## Troubleshooting

### Snapshots are missing from the recording

Check what `takeSnapshot()` or `tryTakeSnapshot()` returns:

| Result | Cause | Fix |
|---|---|---|
| `pool_exhausted` | A slow sink holds every slot of the pool. | Raise `setPoolCapacity()`, or make the sink faster. |
| `partial`, `rejected` | Some or all sinks are stopped. | `start()` the worker, or detach it. |
| `no_sinks` | No sink is attached. | Attach one. |
| `oversize` | A vector outgrew the slot (`tryTakeSnapshot()` only). | Raise `setPayloadCapacity()`. |
| `blocked` | A writer or another snapshot held the write mutex too long (`tryTakeSnapshot()`), or the calling thread holds it itself (both). | Shorten `scopedWrite()` scopes; release guards before a snapshot. |
| `serialize_failed` | A serializer threw inside `tryTakeSnapshot()` (`takeSnapshot()` lets the exception through): a custom serializer failed, or another thread changed a vector without `scopedWrite()`, so it no longer fit. | Fix the serializer; write shared variables inside `scopedWrite()`. |
| `start_failed` | The first `tryTakeSnapshot()` called `startLogging()`, which threw (a sink's `onSchema()`, an allocation); `takeSnapshot()` throws instead. | Fix the sink; the next call retries. |

For totals over time, read the counters from a non real-time thread:

```cpp
const auto channel_stats = channel->stats();
// attempts, accepted, pool_exhausted, dropped_oversize, serialize_failures, ...
const auto sink_stats = worker->stats();
// delivered, errors, last_error, queue_high_water
```

`sink_stats.errors` and `last_error` show a sink that failed to write, for instance on a
full disk.

### Registration throws "once recording started"

The schema froze before you registered the value. Register everything before `startLogging()`
and before the first snapshot. To record a value only some of the time, register it at
start and use `setEnabled()`.

### Values are garbage, or the program crashes during a snapshot

A registered pointer dangles: a local that went out of scope, an element of a vector that
grew, a member of a moved object, or a member declared after the channel. See
[Lifetime of registered variables](#lifetime-of-registered-variables).

### Values that belong together come from different cycles

Another thread writes them separately. Write them in one
[transaction](#writing-values-from-other-threads).

### The MCAP file is truncated or can't be opened

The sink was never stopped: the process ended without `stop()` and without destroying the
worker (a crash, a signal, `std::exit()` with the worker in a local variable), so the file
has no summary and footer. Call `worker->stop()` or `registry.stopAll()`
(`registry.shutdown()` at the end of `main()`) before such an exit.

### The flight recorder wrote no file

The process ended before the dump was written. Stop the worker, or destroy it, before
exiting. If `requestDump()` returned false, an earlier dump was still in progress.

### The flight recorder file is shorter than the window

The ring is too small for the data rate, and the dump reports `truncated`. Raise
`capacity_bytes`. `stats().newest_timestamp - stats().oldest_timestamp` shows how much
history the ring holds now.

### The first iteration of the control loop is slow

The first `takeSnapshot()` or `tryTakeSnapshot()` called `startLogging()`: it allocated
the pool and called the sinks, which for ROS 2 means a DDS write. Call `channel->startLogging()` before the loop.

### `dlopen()` fails with "cannot allocate memory in static TLS block"

data_tamer keeps its one thread-local (8 bytes) in static TLS, so that a real-time thread
never allocates on its first access, also in a plugin. glibc reserves a small surplus of
static TLS for libraries loaded with `dlopen()`, and libraries loaded before it used it
up. Link data_tamer into the executable, or raise the surplus (glibc 2.32 or later):
`GLIBC_TUNABLES=glibc.rtld.optional_static_tls=2048`.

### The ROS 2 publisher process grows in memory

The data topic uses `KeepAll` and a subscriber is slow. Keep the default
`KeepLast(100)`, or another bounded depth.

### PlotJuggler doesn't show the ROS 2 data

The publisher uses `SchemaFormat::Yaml`, which older PlotJuggler releases can't read.
Leave `schema_format` at its default, `SchemaFormat::Text`. A best-effort `data_qos` also
doesn't match reliable subscribers.

## FAQ

### How do I record at different rates?

Use one channel per rate. Each channel takes its own snapshots, and all of them can share
the same sinks.

### Can I log strings?

Yes: `std::string` and `std::string_view` values, members and containers (see
[What you can register](#what-you-can-register)). For real-time snapshots, size
`setPayloadCapacity()` for the longest text.

### Does disabling a value save space?

Yes. A disabled value costs one bit per snapshot and no payload.

### Is `takeSnapshot()` safe on a real-time thread?

No. It can block on a writer, allocate a larger slot, or call `startLogging()`. Use
`tryTakeSnapshot()`, as shown in [Real-time use](#real-time-use).

### Can one sink serve several channels?

Yes. Attach the same worker to each channel, and every channel's schema reaches it once.

### Can I add or remove sinks while logging?

Yes, but not from a custom serializer, and better not from a sink callback (see
[Writing your own sink](#writing-your-own-sink)): change sinks on another thread.

### Is there a more detailed reference?

[llm_user_manual.md](llm_user_manual.md) covers every rule and corner case. It is written
for coding agents, and people can read it too.
