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
- `tools/`: `check_versions.py` (every version number agrees), `abi_check.sh` (the
  libabigail comparison) and `abi_probe.cpp` (the consumer side of that comparison).
- `.github/workflows/`: plain CMake through conan, one ROS 2 job per distribution
  (industrial_ci), asan and tsan presets, Python tests and PyPI release, and the `abi`
  job (libabigail).

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
  size and alignment of every class and struct whose layout is part of the ABI. Record
  any ABI break in the CHANGELOG. See "Versioning and ABI policy".
- `LogChannel::stats()`, `SinkWorker::stats()` and `MCAPRingSink::stats()` are inline
  functions in the headers, marked `DATA_TAMER_INLINE_LOCAL` (hidden visibility,
  `details/abi.hpp`): they fill the `Stats` struct from exported scalar getters, one per
  field, so the struct never crosses the library boundary, and each binary keeps its own
  copy built against its own header (an exported weak copy from an older header would
  build the struct into a newer caller's buffer). Add a counter as a getter in the `.cpp`
  plus a line in `stats()`, never as a field the library writes. Read `accepted` before
  `attempts` in `LogChannel::stats()` (the snapshot path increments them in the opposite
  order, `accepted` with release). The snapshot path may only gain atomic increments
  (today `attempts` on entry and `accepted` when a sink took the snapshot); worker-side
  counters (`delivered`, `queue_high_water`) are kept on the delivery thread.
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

## Versioning and ABI policy

One version number covers the C++ library, the ROS packages and the Python decoder
(2.0.0 until the 2.0.0 tag). It lives in:

- `data_tamer_cpp/CMakeLists.txt`, `project(... VERSION x.y.z)`. The shared library gets
  `VERSION x.y.z` and `SOVERSION x` (the major), so it installs as
  `libdata_tamer.so.x.y.z` with the symlinks `libdata_tamer.so.x` (the SONAME) and
  `libdata_tamer.so`. The compile definition `DATA_TAMER_VERSION` and the CMake package
  version file (non-ROS install, `SameMajorVersion`) derive from it;
- `data_tamer_cpp/package.xml` and `data_tamer_msgs/package.xml`, `<version>`;
- `data_tamer_cpp/conanfile.py`, `version`;
- `python/data_tamer_parser.py`, `__version__`;
- `data_tamer_cpp/CHANGELOG.rst`: the head section is "Unreleased" until the release
  commit renames it to `x.y.z (date)`.

`tools/check_versions.py` compares them and runs as the ctest case `version_consistency`.
Change them together.

The SOVERSION is the ABI generation. Within one major version a newer
`libdata_tamer.so` must keep working with consumers built against any older header of
that major. That is frozen inside 2.x:

- the exported symbols (name, signature, mangling), including the private back-end
  functions that inline templates call (`LogChannel::registerValueImpl`,
  `controlMutex`, `checkValueName`, `schemaFrozen`, `hasCustomType`, `addCustomType`,
  `sharedState`, `TypesRegistry::findOrCreate` and `replace`) and the lock contract
  between them;
- the size, alignment and member offsets of every type in `tests/abi_tests.cpp`:
  by-value structs (`ChannelDefaults`, `MCAPRingOptions`, `ROS2PublisherOptions`,
  `Snapshot`, `Schema`, ...), the classes whose inline code runs in consumers
  (`LoggedValue<T>`, `ChannelSharedState`, `WriteMutex`, `ValuePtr`, `Transaction`) and
  the Pimpl classes (their pointer members). Pinning `Snapshot`, `MCAPRingDump` and
  `Schema` completely is deliberate, although a field appended to them would be safe
  for readers. The exact byte values are for x86_64 only (aarch64 sizes a mutex at 48
  bytes, not 40); what contains a mutex is pinned by relation on every libstdc++
  platform. A new pin with a number needs the number measured on x86_64, and a
  relation if it can differ elsewhere. The `Stats` structs are not on this list: they
  are built inline from exported getters (see Invariants), so their getters are frozen
  instead;
- the vtables of `DataSink` and `CustomSerializer` (see Invariants). abidiff cannot see
  `CustomSerializer`'s in the library, which is why the check also builds a probe;
- the inline protocols: the flag word encoding of `ChannelSharedState`, the
  `Transaction` chain, the `registerValue` templates, the payload encoding and schema
  text (any wire revision, see Invariants).

Adding a function, an overload, a class or an appended enumerator is compatible. Header
defaults (member initializers, default arguments) take effect only in rebuilt
consumers; defaults inside a Pimpl take effect for old binaries too. Say which in the
CHANGELOG.

Changing anything frozen is a break: bump the major version in all the files above
(so `SOVERSION` changes) and record it in the CHANGELOG. Put new state behind a Pimpl
instead of growing a pinned layout. Options structs passed by value are not size-tagged,
so adding a field to one is a break too.

Before the first 2.0.0 tag, a layout change to a pinned struct or class is allowed in
the PR that owns it, with the pins in `abi_tests.cpp` updated in the same commit. The
vtables of `DataSink` and `CustomSerializer` and the inline `Stats` rule (see
Invariants) are frozen already. Do not change the library's default symbol visibility
or mark classes `final` outside the planned visibility change.

### Checking the ABI locally

CI (`.github/workflows/abi.yml`, pull requests and pushes to `V2`) builds the library
from the baseline and from the checkout and compares them with libabigail's `abidiff`,
restricted to the installed headers (`--drop-private-types`; the Pimpl structs are
private). A second comparison covers what consumers compile into their own binaries:
`tools/abi_probe.cpp` derives from `DataSink` and `CustomSerializer`, calls the inline
`stats()` functions and instantiates the `registerValue`, `LoggedValue` and guard
templates, so that vtables and inline layouts that the library never defines are in the
debug info (keep it to the stable API). The check fails when an exported function or
variable is removed or changed (a vtable, a by-value struct that grew) unless the SONAME
differs; added ones pass. The baseline is the highest release tag of the same major
version, or, while there is none (before 2.0.0), the merge base with the base branch
(the previous commit on a push to `V2`). Locally:

```bash
sudo apt install abigail-tools libzstd-dev liblz4-dev     # abidiff
tools/abi_check.sh                         # automatic baseline, as in CI
tools/abi_check.sh --baseline-ref 2.0.0    # any tag, branch or commit
tools/abi_check.sh --baseline-dir /path/to/exported/tree
```

The script builds both sides (plain CMake, shared, Debug, no ROS) in a temporary
directory and prints the abidiff report. A removed function counts only if the baseline
library defined it strongly: the weak copies of `std::` and other inline functions
that the library exports are not API. A change to `tools/abi_probe.cpp` that uses new
API still gets its consumer-side comparison, with the baseline's own probe. The ROS 2
sink is not covered: its ABI also follows the rclcpp release, and its pins are in
`abi_tests.cpp`. Before 2.0.0, a layout change that the PR owns is accepted with
`--allow-break` locally and the `abi-break` label on the pull request (never for the
frozen vtables); once a 2.x tag exists the label no longer works and only a major
version bump passes. The check also runs on pushes to `V2`, against the previous
commit.

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
