# DataTamer user manual

This manual starts where the [README](../README.md) stops. The chapters explain how to use
each part of the library. [Troubleshooting](#troubleshooting) starts from a symptom, and
the [FAQ](#faq) collects short answers that fit nowhere else.

- [Concepts](#concepts)
- [Registering values](#registering-values)
- [Writing values from other threads](#writing-values-from-other-threads)
- [Real-time use](#real-time-use)
- [Sinks](#sinks)
- [Reading the data](#reading-the-data)
- [Building and linking](#building-and-linking)
- [Troubleshooting](#troubleshooting)
- [FAQ](#faq)

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
size and signedness, so `long long` works; `long double` is not supported, use `double`. The wire format has no
string type, so don't log text: a `std::string` would be recorded as one number per
character.

### When you can register

Register everything before the first snapshot. The schema freezes at `channel->startLogging()`,
or at the first `takeSnapshot()` that finds a sink attached, and registering a new name
after that throws `std::runtime_error`. A name you unregistered can be registered again
after the freeze, but only with its original type, because it reuses its slot.

### Lifetime of registered variables

`registerValue("x", &x)` borrows the pointer and reads it at every snapshot until
`unregister()` returns or the channel is destroyed. A dangling pointer is not detected, so:

- declare registered variables before the channel, locals and class members alike;
- register a `std::vector` itself, not one of its elements, because growing the vector
  moves them;
- don't register a member of an object that is later moved or copied.

If a value has no natural owner, let the channel create it:

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
PlotJuggler shows as a tree. `DataTamer::JoinNames()` drops empty components, so you don't
end up with `loco//LF/x`:

```cpp
#include "data_tamer/names.hpp"

const auto name = DataTamer::JoinNames("loco/", leg_name, "x");
assert(DataTamer::IsCanonicalName(name));  // optional check
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

The [README example](../README.md#how-to-register-custom-types) puts a `TypeDefinition()`
function in the namespace of the type. For types whose namespace you don't own, such as
Eigen or a vendor SDK, specialize `DataTamer::TypeDefinitionTrait` instead:

```cpp
#include "data_tamer/custom_types.hpp"

template <>
struct DataTamer::TypeDefinitionTrait<Eigen::Vector3d>
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
with one name in a channel make `registerValue()` throw.

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

`tryTakeSnapshot()` exists to be lock-free: it never blocks on a lock and never
allocates, so a real-time loop can call it every cycle. Do the work that allocates before
the loop, and call `tryTakeSnapshot()` inside it:

```cpp
using namespace std::chrono_literals;

// before the loop, on a normal thread
channel->setPoolCapacity(200ms, 1ms);  // absorb a 200 ms sink stall at 1 kHz
channel->startLogging();                    // freeze the schema, allocate, announce to the sinks

// inside the loop
const auto result = channel->tryTakeSnapshot();
if(result != DataTamer::SnapshotResult::ok)
{
  ++snapshot_failures;  // count it, report it from another thread
}
```

Where `takeSnapshot()` would wait for a writer or grow a slot, `tryTakeSnapshot()` gives up
and returns `blocked` or `oversize`. It reports every failure as a `SnapshotResult` and
doesn't throw. Several threads may take snapshots of one channel: they take turns on its
write mutex, and `tryTakeSnapshot()` returns `blocked` while another snapshot holds it.

The pool has 64 slots by default, which is 64 ms at 1 kHz. If a sink stalls longer than
that (a disk flush can take 200 ms), new snapshots are dropped with `pool_exhausted`.
`setPoolCapacity(stall, period)` sizes the pool for the stall you want to absorb.
`setPayloadCapacity(bytes)` reserves room for vectors that grow at run time. Both must be
called before `startLogging()`.

## Sinks

A sink is created with its `create()` function, which returns a
`std::shared_ptr<SinkWorker>`. Attach it to one or more channels with `addDataSink()`, and
reach the sink's own functions with `worker->as<SinkType>()`.

### Recording to MCAP files

```cpp
#include "data_tamer/sinks/mcap_sink.hpp"

auto worker = DataTamer::MCAPSink::create("run.mcap");
channel->addDataSink(worker);
```

By default `MCAPSink` starts a new numbered file every 10 minutes (`run.mcap`,
`run_1.mcap`, `run_2.mcap`, ...), so no file grows forever and nothing is lost. To change
it:

```cpp
auto& sink = worker->as<DataTamer::MCAPSink>();
sink.setMaxTimeBeforeReset(std::chrono::seconds(0));  // one file, never reset
// or: sink.setCreateNewFileOnReset(false);           // keep only the last 10 minutes
```

`setCreateNewFileOnReset(false)` overwrites the file at every reset, so use it only for a
recording you don't keep. If the next file cannot be opened at a reset, the failure is
counted once in `worker->errors()` and the current file keeps recording.

### Flight recorder

`MCAPRingSink` keeps the last `window` of every attached channel in RAM and writes an MCAP
file only when you call `requestDump()`, for instance after a fault:

```cpp
#include "data_tamer/sinks/mcap_ring_sink.hpp"

DataTamer::MCAPRingOptions options;
options.filepath = "fault.mcap";            // fault_1.mcap, fault_2.mcap, ...
options.window = std::chrono::seconds(5);   // history before the trigger
options.capacity_bytes = size_t(64) << 20;  // the sink uses about twice this
auto worker = DataTamer::MCAPRingSink::create(options);
channel->addDataSink(worker);

// from any thread, real-time ones included: keep also 2 s after the trigger
(void)worker->as<DataTamer::MCAPRingSink>().requestDump(std::chrono::seconds(2));
```

The dump covers `[trigger - window, trigger + post_trigger]` in snapshot time, so it
behaves the same in simulation, in a replay and on the robot. `requestDump()` returns false
while an earlier dump is still in progress (the result is `[[nodiscard]]`). `setDumpCallback()` reports every file written
or failed. [T04_flight_recorder.cpp](../data_tamer_cpp/examples/T04_flight_recorder.cpp)
is a complete example.

### Publishing on ROS 2

`ROS2PublisherSink` publishes the schemas on `<prefix>/schemas` and one
`data_tamer_msgs/Snapshot` per sample on `<prefix>/data`. To publish fewer messages,
aggregate the samples into batches on `<prefix>/data_batch`:

```cpp
#include "data_tamer/sinks/ros2_publisher_sink.hpp"

DataTamer::ROS2PublisherOptions options;
options.aggregate = true;
options.max_batch_size = 100;                              // snapshots per message
options.max_batch_delay = std::chrono::milliseconds(100);  // or this old
auto worker = DataTamer::ROS2PublisherSink::create(node, "/robot", options);
channel->addDataSink(worker);
```

`startLogging()` publishes the schemas, so call it before the control loop. The data topic keeps the last 100 messages (`options.data_qos`): a slow
subscriber loses old messages instead of growing the publisher's memory.
[ros2_publisher.cpp](../data_tamer_cpp/examples/ros2_publisher.cpp) and
[ros2_subscriber.py](../python/ros2_subscriber.py) show both ends.

### Writing your own sink

Derive from `DataTamer::DataSink`, implement `onSchema()` and `onSnapshot()`, and wrap it in
a `SinkWorker`, which owns the thread that calls them:

```cpp
class CountingSink : public DataTamer::DataSink
{
protected:
  void onSchema(const DataTamer::Schema& schema) override { /* once per channel */ }
  void onSnapshot(const DataTamer::SnapshotRef& snapshot) override { ++count_; }

private:
  size_t count_ = 0;
};

auto worker = DataTamer::SinkWorker::create<CountingSink>();
channel->addDataSink(worker);
```

Keep `onSnapshot()` short: while it holds a snapshot, that slot of the channel's pool stays
busy for every sink. For slow work, copy the snapshot (`DataTamer::Snapshot copy =
*snapshot;`) and hand the copy to your own thread. Override `onStop()` and `onStart()` if
the sink has files to close and reopen.

### Shutdown

Stop every sink before the program exits. `stop()` delivers what is queued, then lets the
sink finish: `MCAPSink` closes its file, `MCAPRingSink` writes a pending dump,
`ROS2PublisherSink` publishes its partial batch.

```cpp
worker->stop();
// or, for every sink of a registry:
DataTamer::ChannelsRegistry::Global().stopAll();
```

Destroying a running worker stops it too.

### Registry defaults

`ChannelsRegistry` creates channels by name, applies default settings to each new channel
and attaches the default sinks to every channel, existing ones included:

```cpp
using namespace std::chrono_literals;

auto& registry = DataTamer::ChannelsRegistry::Global();
DataTamer::ChannelDefaults defaults;
defaults.pool_stall_tolerance = 200ms;  // with the next line: 200 slots per channel
defaults.pool_snapshot_period = 1ms;
registry.setChannelDefaults(defaults);  // for channels created from now on
registry.addDefaultSink(DataTamer::MCAPSink::create("run.mcap"));
auto channel = registry.getChannel("controller");
```

## Reading the data

[PlotJuggler](https://github.com/facontidavide/PlotJuggler) 3.8.2 or later opens the MCAP
files and subscribes to the ROS 2 topics.

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
files one after the other.

In C++, copy the single header
[data_tamer_parser.hpp](../data_tamer_cpp/include/data_tamer_parser/data_tamer_parser.hpp)
into your project: it has no dependencies.
[mcap_reader.cpp](../data_tamer_cpp/examples/mcap_reader.cpp) shows how to use it,
including `SplitMcapMessage()` and the checks on a corrupt message. The
format is specified in [wire_format.md](wire_format.md).

## Building and linking

The library is built as C++20, and its public headers need only C++17.

The shared library has the SONAME `libdata_tamer.so.2`, so a program built against any 2.x
release runs with any later 2.x library, and `find_package(data_tamer 2 REQUIRED)` accepts
any 2.x. Defaults that live in headers change only when you rebuild.

A header that only names DataTamer types, such as a `LogChannel&` parameter or a
`std::shared_ptr<SinkWorker>` member, can include `data_tamer/fwd.hpp` instead of the full
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
| `not_started` | `startLogging()` was not called (`tryTakeSnapshot()` only). | Call it before the loop. |

For totals over time, read the counters from a non real-time thread:

```cpp
const auto channel_stats = channel->stats();
// attempts, accepted, pool_exhausted, dropped_oversize, dropped_by_sink, ...
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

The sink was never stopped, so the file has no summary and footer. Call `worker->stop()`
or `registry.stopAll()` before the program exits.

### The flight recorder wrote no file

The process ended before the dump was written. Stop the worker, or destroy it, before
exiting. If `requestDump()` returned false, an earlier dump was still in progress.

### The flight recorder file is shorter than the window

The ring is too small for the data rate, and the dump reports `truncated`. Raise
`capacity_bytes`. `stats().newest_timestamp - stats().oldest_timestamp` shows how much
history the ring holds now.

### The first iteration of the control loop is slow

The first `takeSnapshot()` called `startLogging()`: it allocated the pool and called the
sinks, which for ROS 2 means a DDS write. Call `channel->startLogging()` before the loop.

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

No. The wire format has no string type.

### Does disabling a value save space?

Yes. A disabled value costs one bit per snapshot and no payload.

### Is `takeSnapshot()` safe on a real-time thread?

No. It can block on a writer, allocate a larger slot, or call `startLogging()`. Use
`tryTakeSnapshot()`, as shown in [Real-time use](#real-time-use).

### Can one sink serve several channels?

Yes. Attach the same worker to each channel, and every channel's schema reaches it once.

### Can I add or remove sinks while logging?

Yes, but not from a sink callback or a custom serializer.

### Is there a more detailed reference?

[llm_user_manual.md](llm_user_manual.md) covers every rule and corner case. It is written
for coding agents, and people can read it too.
