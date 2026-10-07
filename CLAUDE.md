# CLAUDE.md

data_tamer logs numeric variables of a C++ program as periodic snapshots and hands them
to sinks (MCAP files, a RAM flight recorder, ROS 2 topics). This file is for work on the
library itself. To use the library from another code base, read
[docs/llm_user_manual.md](docs/llm_user_manual.md). The byte format is specified in
[docs/wire_format.md](docs/wire_format.md).

## Documentation

Keep the documentation current in every PR, without being asked. A PR that changes
behaviour, API, defaults, options, build steps or the wire format updates, in the same PR:

- README.md;
- docs/llm_user_manual.md;
- docs/wire_format.md, together with the golden vectors and both decoders;
- code comments and doc comments in the public headers;
- this CLAUDE.md.

A PR that leaves any of them stale is incomplete. User-visible changes also get an entry
under "Unreleased" in `data_tamer_cpp/CHANGELOG.rst`.

## Repository layout

- `data_tamer_cpp/`: the library. ROS 2 package `data_tamer_cpp`, CMake target `data_tamer`.
  - `include/data_tamer/`: public headers. `channel.hpp` (LogChannel, SnapshotResult),
    `data_tamer.hpp` (ChannelsRegistry), `data_sink.hpp` (DataSink, SinkWorker, Snapshot,
    SnapshotRef), `logged_value.hpp`, `names.hpp`, `types.hpp` (Schema, BasicType),
    `custom_types.hpp`, `values.hpp`, `fwd.hpp`, `contrib/SerializeMe.hpp`
    (TypeDefinitionTrait, serialization), `details/` (pool, write mutex, spin pause, shared state:
    public because templates use them), `sinks/`.
  - `include/data_tamer_parser/data_tamer_parser.hpp`: standalone C++ decoder, no
    dependency on the library.
  - `src/`: implementation; `src/sinks/` for MCAPSink, MCAPRingSink, ROS2PublisherSink.
  - `tests/`: one gtest binary, `datatamer_test`, plus compile-only targets.
  - `examples/`: T01 to T04, `mcap_1m_per_sec`, `mcap_reader`, `ros2_publisher` (ROS only).
  - `benchmarks/`: built only when Google Benchmark is found.
  - `3rdparty/`: vendored MCAP.
- `data_tamer_msgs/`: ROS 2 messages `Schema`, `Schemas`, `Snapshot`, `SnapshotBatch`.
- `python/`: `data_tamer_parser.py`, the reference decoder (standard library only),
  packaged as `data-tamer-parser`; its tests; `ros2_subscriber.py`.
- `docs/`: `wire_format.md` and its golden vectors in `wire_format/vectors/`;
  `benchmarks/` and `reviews/` are dated records.
- `.github/workflows/`: plain CMake through conan, one ROS 2 job per distribution
  (industrial_ci), asan and tsan presets, Python tests and PyPI release.

## Build and test

ROS 2, with the repository under `src/` of a colcon workspace:

```bash
source /opt/ros/jazzy/setup.bash
colcon build --packages-up-to data_tamer_cpp
colcon test --packages-select data_tamer_cpp
colcon test-result --verbose
```

Under ROS the whole gtest binary is one ctest entry. Run a subset directly:

```bash
source install/setup.bash
./build/data_tamer_cpp/tests/datatamer_test --gtest_filter='WireFormat.*'
```

Plain CMake without ROS, from `data_tamer_cpp/` (needs GTest, zstd and lz4, e.g.
`libgtest-dev libzstd-dev liblz4-dev`). The presets set `DATA_TAMER_BUILD_ROS=OFF`;
without them, pass that option yourself when ROS is sourced, because CMake builds the
ROS flavour whenever it finds `ament_cmake`.

```bash
cmake --preset debug            # build/debug: tests on, examples and benchmarks off
cmake --build --preset debug
ctest --preset debug            # every gtest case is its own ctest entry
ctest --preset debug -R 'WireFormat'
```

Other presets: `release` (adds examples and benchmarks), `asan`, `tsan` (runs the tests
through `tests/run_under_tsan.sh`). CI runs `asan` and `tsan`.

Python decoder tests, from the repository root (ctest runs them too, as
`python_reference_decoder`). The `iter_mcap` tests are skipped unless the `mcap` package
is installed (`pip install "./python[mcap]"`).

```bash
python3 -m unittest discover -s python -p 'test_*.py'
```

Regenerate the golden vectors after a deliberate format change, then review the diff
and update `docs/wire_format/vectors/expected.json` by hand:

```bash
DATA_TAMER_UPDATE_GOLDEN=1 \
  ./build/debug/tests/datatamer_test --gtest_filter='WireFormat.*'
```

## Invariants

- The library and the tests build as C++20. Public headers must compile as strict C++17:
  the `public_headers_cxx17` target (`tests/public_headers_cxx17.cpp`) includes every
  core public header, so add new headers there. `sinks/ros2_publisher_sink.hpp` is
  exempt (rclcpp sets its own standard). Examples build as C++17 consumers.
- The library builds with `-Wall -Wextra -Wpedantic -Wconversion -Wno-sign-conversion
  -Werror`.
- The real-time path allocates nothing, takes no blocking lock, does no I/O and does not
  throw. It covers `tryTakeSnapshot()` and what it reaches (`SnapshotPool::tryAcquire`,
  `WriteMutex::tryLockWithSpin`, `ValuePtr` serialization, `SinkWorker::tryPush` and
  its per-channel single-producer queue),
  scalar `LoggedValue::set()`/`get()`, `trySetEnabled()` and
  `MCAPRingSink::requestDump()`. Tests assert it with `AllocCounter::Scope`
  (`tests/alloc_counter.hpp`): add one when you touch these paths. `takeSnapshot()` may
  block and grow a slot; keep the two variants distinct.
- `onSchema()`/`onSnapshot()`/`onStop()`/`onStart()` are serialized by the SinkWorker,
  and control operations (registration, sinks, `prepare()`) wait for them. Never call a
  control operation from a sink callback, a serializer or inside `scopedWrite()`.
  `stop()` runs `onStop()` once per stop, after the last delivery, and `start()` after a
  stop runs `onStart()`: a sink that needs to finish (close, flush, dump) or reopen does
  it there.
- A change to the schema text, the payload encoding, the schema hash or the MCAP and ROS
  message layout is a format revision. Update `docs/wire_format.md`, regenerate the
  vectors, update `python/data_tamer_parser.py` and `data_tamer_parser.hpp`, and bump
  `SCHEMA_VERSION` or `SCHEMA_YAML_VERSION` in `types.hpp` when the text changes. The
  `WireFormat.*` tests and the Python tests fail until all of them agree.
- LogChannel, ChannelsRegistry, SinkWorker, TypesRegistry, MCAPSink, MCAPRingSink and
  ROS2PublisherSink keep all their state behind a Pimpl (LogChannel's only other base is
  `enable_shared_from_this`). Add members to the Pimpl. `tests/abi_tests.cpp` pins the
  sizes of the sinks, SinkWorker, LogChannel, SnapshotRef, DataSink and ChannelDefaults.
  Record any ABI break in the CHANGELOG.
- The vtables of `DataSink` and `CustomSerializer` are frozen for 2.x: users derive from
  them and the library calls through vtables compiled into their binaries. Add no virtual
  function and no data member; new behaviour goes into non-virtual functions or a
  separate interface.
- Only `SnapshotPool::adopt()` builds a SnapshotRef from a pool slot; the constructor is
  private.
- `MCAPRingStats` never crosses the library boundary: `MCAPRingSink::stats()` is inline
  and fills it from one exported getter per field. A new field gets a new getter;
  existing getters stay.
- Include what you use. Headers that only name data_tamer types include `fwd.hpp`, which
  must declare each type with the same class or struct key as its definition (the
  `fwd_header_*` targets check it). `data_tamer.hpp` includes only `fwd.hpp`, so code
  using LogChannel includes `channel.hpp`. Keep heavy includes out of public headers.
- `3rdparty/` is vendored: do not edit it. pre-commit excludes it.

## Conventions

- Format with the repository `.clang-format` (Google based, 90 columns, braces on their
  own line). Run `pre-commit run --files <changed files>`: it pins clang-format 23.1.1,
  and the distribution clang-format 18 formats some files differently. Avoid
  `--all-files`, which rewrites files unrelated to your change.
- Tests: gtest, `TEST(Suite, BehaviourInCamelCase)`, new files listed in
  `DATATAMER_TEST_SOURCES` (`tests/CMakeLists.txt`). Helpers live in
  `tests/test_sinks.hpp` (`Attached<T>`, `channelWith`, `acceptedUntilExhausted`) and
  `tests/mcap_test_utils.hpp` (`tempPath`, `countMessages`). Wait for delivery with `SinkWorker::drain()` or
  `Delivery::Manual`, never with sleeps.
- Commits: short imperative subject, optionally prefixed by the component
  (`MCAPSink: roll over into new files by default`). The body says why, and what was
  tested.
