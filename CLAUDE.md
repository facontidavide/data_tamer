# CLAUDE.md

data_tamer logs numeric variables of a C++ program as periodic snapshots and hands them
to sinks (MCAP files, a RAM flight recorder, ROS 2 topics). This file is for work on the
library itself. To use the library from another code base, read
[docs/llm_user_manual.md](docs/llm_user_manual.md). People read
[docs/USER_MANUAL.md](docs/USER_MANUAL.md), which has the same rules in less detail plus
troubleshooting and a FAQ. The byte format is specified in
[docs/wire_format.md](docs/wire_format.md).

## Documentation

Keep the documentation current in every PR, without being asked. A PR that changes
behaviour, API, defaults, options, build steps or the wire format updates, in the same PR:

- README.md;
- docs/USER_MANUAL.md;
- docs/llm_user_manual.md;
- docs/MIGRATION.md, while 2.0 is unreleased: every source break and silent behaviour
  change since 1.0.4;
- docs/wire_format.md, together with the golden vectors and every decoder;
- code comments and doc comments in the public headers;
- this CLAUDE.md.

A PR that leaves any of them stale is incomplete. User-visible changes also get an entry
under "Unreleased" in `data_tamer_cpp/CHANGELOG.rst`.

## Repository layout

- `data_tamer_cpp/`: the library. ROS 2 package and CMake project `data_tamer_v2`, which
  installs side by side with the released 1.x package `data_tamer_cpp`: no installed file
  may collide with 1.x (`share/data_tamer_cpp`, `include/data_tamer`,
  `include/data_tamer_parser`, `lib/libdata_tamer.so`), so under ROS the library is
  `libdata_tamer_v2.so`; the ROS 2 CI jobs compare the install with the released 1.x
  binary package (`tools/check_ros_install.sh`). CMake target `data_tamer`, exported as
  `data_tamer_v2::data_tamer` under ROS and `data_tamer::data_tamer` (package
  `data_tamer`) otherwise.
  - `include/data_tamer_v2/`: public headers. `channel.hpp` (LogChannel, SnapshotResult,
    `accepted()`),
    `data_tamer.hpp` (ChannelsRegistry), `data_sink.hpp` (DataSink, SinkWorker,
    SinkWorkerOf, Snapshot, SnapshotRef), `logged_value.hpp`, `names.hpp`, `types.hpp`
    (Schema, BasicType), `custom_types.hpp`, `values.hpp`, `fwd.hpp`, `decode.hpp`
    (`ToSnapshotView()` of a Snapshot for the parser), `mcap_reader.hpp`
    (`ForEachSnapshot()`, `ForEachValue()`: header-only MCAP file reading),
    `contrib/SerializeMe.hpp`
    (TypeDefinitionTrait, serialization), `details/` (pool, `WriteTransaction` and the
    guards in `locked_reference.hpp`, `abi.hpp`: public because inline code uses them),
    `sinks/`.
  - `include/data_tamer_v2/data_tamer_parser.hpp`: standalone C++ decoder, no
    dependency on the library.
  - `src/`: implementation; `src/sinks/` for MCAPSink, MCAPRingSink, ROS2PublisherSink.
    Library-private headers: `shared_state.hpp` (ChannelSharedState, AtomicWordTable),
    `write_mutex.hpp`, `spin_pause.hpp`, `waiter_count.hpp`, `realtime_sanitizer.hpp`
    (`details::AllowedRealTimeCall`). Not installed; tests and benchmarks have `src/` on
    their include path.
  - `tests/`: the gtest binary `datatamer_test`; `serialize_me_big_endian_test`,
    SerializeMe built as for a big endian host; compile-only targets (with GCC,
    `text_registration_without_p0522`: text registers without relaxed template template
    matching, as with Clang 18); compile-fail
    probes in `tests/compile_fail/`; in Linux shared builds, `dlopen_tls_test`, which
    loads libdata_tamer with a plugin through `dlopen()` and counts `malloc()` calls;
    with Clang 20 or later, the `-Wfunction-effects` probe `nonblocking_caller.cpp` and,
    on Linux, `rtsan_test` (`rtsan_tests.cpp`, built with `-fsanitize=realtime`).
    The gtest programs come from `data_tamer_add_gtest()`.
  - `examples/`: T01 to T08, the progressive joint-controller tutorial (see its
    `README.md`); `mcap_1m_per_sec` and `ros2_publisher` (ROS only) are separate demos.
  - `benchmarks/`: built only when Google Benchmark is found. `parser_benchmark` times
    the standalone parser (`ParseSnapshot()`) on snapshots the library takes.
  - MCAP is not in the tree. Under ROS 2 it is `mcap_vendor`. Otherwise CMake downloads
    the MCAP C++ 2.1.3 release archive of foxglove/mcap (tag `releases/cpp/v2.1.3`,
    pinned by SHA256) with FetchContent, as `data_tamer_mcap`, unless
    `DATA_TAMER_USE_SYSTEM_MCAP` finds an installed package (conan). The downloaded
    headers and MIT `LICENSE` are installed in `include/data_tamer_v2_mcap/mcap`, an
    include directory of the target that holds nothing else.
  - `cmake/`: `Findzstd.cmake` and `Findlz4.cmake` (the imported targets
    `data_tamer::zstd` and `data_tamer::lz4`, adapted from Meta's modules: keep their
    Apache-2.0 notice), the installed config `data_tamerConfig.cmake.in`, and
    `data_tamerConfig-ros.cmake`, the `data_tamer` package of a ROS 2 install.
  - `test_package/`: a `find_package(data_tamer 2)` consumer, built by `conan create` and
    by the CI install job.
- `data_tamer_msgs/`: ROS 2 messages `Schema`, `Schemas`, `Snapshot`, `SnapshotBatch`.
- `python/`: `data_tamer_parser.py`, the reference decoder (standard library only),
  packaged as `data-tamer-parser`; its tests; `ros2_subscriber.py`. In the decoder,
  `_parse_snapshot_reference()` walks the schema value by value and is the
  specification; `parse_snapshot()` and `parse_snapshot_values()` decode through plans
  compiled per schema and active mask (`struct` formats, cached on the `Schema`), which
  fall back to the walker for what they do not compile. The caches are bounded: at most
  `_MAX_PLANS` masks per schema (the walker decodes further ones), `_PLAN_BUDGET` values
  per plan, and `_MAX_CACHED_NAMES` names per plan for its vectors and its last layout.
  A `string` (text) is a `_TEXT` op, and a dynamic `string[]` is walked.
  `test_fast_path.py` checks them against it, values and error messages: random schemas,
  the golden vectors, the 1.0.4 recording and every decode of the other tests.
  `benchmark_decode.py` times the three.
- `data_tamer_foxglove/`: strict ESM TypeScript snapshot decoder, no runtime dependencies;
  schema versions 4 and 5 (line format and YAML), bounded mask/layout plans and ROS 2 message helpers. Its
  `test/` checks golden vectors, the 1.0.4 recording and generated Python differential
  cases; `bench/benchmark.mjs` times flat and nested output.
  - `extension/`: Foxglove desktop/web schema converters (Node 22), bundling
    `../src/` directly. `src/converters.ts` shares the catalog and converts
    snapshots/header stamps and inspectable batches; `src/index.ts` registers
    them. `test/` checks golden bytes with fake MessageEvents, without the app.
- `docs/`: `USER_MANUAL.md` (for people), `llm_user_manual.md` (for coding agents),
  `MIGRATION.md` (1.x to 2.0), `wire_format.md` and its golden vectors in
  `wire_format/vectors/` (`v4/`: a 1.0.4 recording).
- `tools/`: `check_versions.py` (every version number agrees), `abi_check.sh` (the
  libabigail comparison) and `abi_probe.cpp` (the consumer side of that comparison),
  `check_ros_install.sh` (the checks of a ROS 2 install, see "Build and test"),
  `release_notes.py` (a version's CHANGELOG section as Markdown, for the GitHub release)
  and its test `test_release_notes.py` (ctest case `release_notes`).
- `.github/workflows/`: plain CMake through conan plus `conan create`, the `install` job
  (static and shared installs, `test_package/` built against each, Ubuntu 22.04 and
  24.04), one ROS 2 job per distribution (industrial_ci; its `AFTER_SCRIPT` runs
  `tools/check_ros_install.sh` on the install space), asan, tsan and rtsan presets,
  Python tests and PyPI release, the `abi` job (libabigail), and `release.yml`, which
  creates the GitHub release when a tag `X.Y.Z` is pushed (see "Versioning and ABI
  policy").

## Build and test

ROS 2, with the repository under `src/` of a colcon workspace:

```bash
source /opt/ros/jazzy/setup.bash
colcon build --packages-up-to data_tamer_v2
colcon test --packages-select data_tamer_v2
colcon test-result --verbose
```

`--cmake-args -DBUILD_TESTING=OFF` builds no test target (as bloom does); the gtest
packages are `<test_depend>` in `package.xml`. Under ROS, MCAP comes from `mcap_vendor`,
linked PUBLIC, and the install also provides `share/data_tamer/cmake` so that
`find_package(data_tamer 2)` works there.

The ROS 2 CI jobs then run `tools/check_ros_install.sh` through industrial_ci's
`AFTER_SCRIPT` (`rosenv tools/check_ros_install.sh`, which sources the install space).
It builds and runs `test_package/` (`find_package(data_tamer 2)`) against the install,
and fails when `data_tamer_v2` or `data_tamer_msgs` install a file that the released
`ros-$ROS_DISTRO-data-tamer-cpp` binary package (downloaded with `apt-get download`, not
installed) also has: bloom puts both in `/opt/ros/$ROS_DISTRO`. A distribution that does
not release 1.x (Rolling) has no such package, and the comparison is skipped. To run it,
source the workspace (`source install/setup.bash`) and call the script. The Rolling job
builds on Ubuntu resolute against `ros2-testing`, as the build farm does.

Under ROS the whole gtest binary is one ctest entry. Run a subset directly:

```bash
source install/setup.bash
./build/data_tamer_v2/tests/datatamer_test --gtest_filter='WireFormat.*'
```

Plain CMake without ROS, from `data_tamer_cpp/` (needs GTest, zstd and lz4, e.g.
`libgtest-dev libzstd-dev liblz4-dev`). The presets set `DATA_TAMER_BUILD_ROS=OFF` and
`DATA_TAMER_WERROR=ON`; without them, pass `DATA_TAMER_BUILD_ROS=OFF` yourself when ROS
is sourced, because a top-level build is the ROS flavour whenever it finds
`ament_cmake`. `DATA_TAMER_WERROR` is OFF by default so that a release builds with
newer compilers; CI turns it on everywhere.

```bash
cmake --preset debug            # build/debug: tests and examples on, benchmarks off
cmake --build --preset debug
ctest --preset debug            # every gtest case is its own ctest entry
ctest --preset debug -R 'WireFormat'
```

Other presets: `release` (adds benchmarks), `asan`, `tsan` (runs the tests through
`tests/run_under_tsan.sh`), `rtsan` (`clang++-21`: builds only `rtsan_test` and the
`-Wfunction-effects` probe, and runs them; it fails when the compiler has neither).
The other presets build the examples and run the tutorials T01 to T08 as tests. CI
runs `asan`, `tsan` and `rtsan`. Any Clang 20 or later build without another sanitizer
builds the RTSan (Linux only) and function-effects tests too. RTSan prints its report
through llvm-symbolizer, which hangs on an offline machine where `DEBUGINFOD_URLS` is
set (Ubuntu's default): `rtsan_tests.cpp` unsets it, unset it for your own runs.

Install check, as the CI `install` job does it (repeat with `-DBUILD_SHARED_LIBS=ON`):

```bash
cmake -S . -B /tmp/dt -DDATA_TAMER_BUILD_TESTS=OFF -DDATA_TAMER_BUILD_EXAMPLES=OFF
cmake --build /tmp/dt && cmake --install /tmp/dt --prefix /tmp/dt-prefix
cmake -S test_package -B /tmp/dt-consumer -DCMAKE_PREFIX_PATH=/tmp/dt-prefix
cmake --build /tmp/dt-consumer && /tmp/dt-consumer/test_package
```

`conan create . --build=missing -o "data_tamer/*:tests=False"` builds the conan package
(MCAP from conan's `mcap`, `DATA_TAMER_USE_SYSTEM_MCAP=ON`) and runs `test_package/`.
As a subproject (`add_subdirectory()`, FetchContent) the library has no install rules
(`DATA_TAMER_INSTALL`), no ROS flavour, tests or examples by default, and defines no
global target besides `data_tamer` (the MCAP, zstd and lz4 targets are imported).

Python decoder tests, from the repository root (ctest runs them too, as
`python_reference_decoder`). The `iter_mcap` tests are skipped unless the `mcap` package
is installed (`pip install "./python[mcap]"`).

```bash
python3 -m unittest discover -s python -p 'test_*.py'
```

TypeScript decoder (Node 22, Python 3.9+ for the differential oracle):

```bash
cd data_tamer_foxglove
npm ci
npm run typecheck             # strict tsc --noEmit
npm test                      # builds, generates fixed-seed Python oracle, node --test
npm run bench                 # decodeFlat/decodeMessage, ns per snapshot
```

The recording test uses the development dependency `@mcap/core`. The Python generator
runs with `python3 -I`, imports the repository reference explicitly, and needs only the
standard library.

Foxglove extension (Node 22; no decoder install or build required), from the
repository root:

```bash
cd data_tamer_foxglove/extension
npm ci                        # offline: add --offline --no-audit --no-fund (cached packages)
npm test                      # type-check/compile to .test-dist, then node --test
npm run package               # production build + data_tamer.data-tamer-foxglove-2.0.0.foxe
npm run local-install         # optional desktop install on a development machine
```

`config.ts` must remain valid CommonJS JavaScript: the CLI loads that filename
without transpiling it. Its webpack alias resolves the decoder's `.js` imports
to `.ts` sources; no parser `dist/` is needed. The package is created in the
extension directory.

Regenerate the golden vectors after a deliberate format change, then review the diff
and update `docs/wire_format/vectors/expected.json` by hand (`vectors/v4/` is a file
recorded by data_tamer 1.0.4: it is never regenerated):

```bash
DATA_TAMER_UPDATE_GOLDEN=1 \
  ./build/debug/tests/datatamer_test --gtest_filter='WireFormat.*'
```

## Invariants

- The library and the tests build as C++20. Public headers must compile as strict C++17:
  the `public_headers_cxx17` target (`tests/public_headers_cxx17.cpp`) includes every
  core public header, so add new headers there. `sinks/ros2_publisher_sink.hpp` is
  exempt (rclcpp sets its own standard). Examples build as C++17 consumers.
- The library builds with `-Wall -Wextra -Wpedantic -Wconversion -Wno-sign-conversion`,
  and `-Werror` with `DATA_TAMER_WERROR=ON` (presets and CI). The MCAP headers are a
  SYSTEM include directory.
- The real-time path allocates nothing, takes no blocking lock, does no I/O and does not
  throw. It covers `tryTakeSnapshot()` once logging started and what it reaches
  (`SnapshotPool::tryAcquire`, `WriteMutex::tryLockWithSpin`, `ValuePtr` serialization,
  `SinkWorker::tryPush` and its per-channel queue, filled by one producer at a time
  under the channel's write mutex), scalar `LoggedValue::set()`/`get()`,
  `trySetEnabled()`, `accepted()` and `MCAPRingSink::requestDump()`. Tests assert it
  with `AllocCounter::Scope` (`tests/alloc_counter.hpp`): add one when you touch these
  paths. Before logging started, `tryTakeSnapshot()` without a sink reads the atomic
  `sink_count` and returns `no_sinks`, real-time as well; with a sink, its first call
  runs `startLogging()` (a throw returns `start_failed`) and is not real-time, like
  `takeSnapshot()`: `details::notifyBlockingCall()` reports it to RTSan.
  `takeSnapshot()` may block and grow a slot; keep the two variants distinct. Nothing is
  thrown out of the real-time path: `tryTakeSnapshot()` is `noexcept`, and
  `takeSnapshotImpl()` catches
  around the sizing and serialization of the values, where a serializer may throw (user
  code, or SerializeMe when a container changed size outside `scopedWrite()`), and
  returns `serialize_failed`; `takeSnapshot()` rethrows. That catch, on a fault path, is
  the only place a throw may happen (the throw allocates). The real-time path makes no
  futex call unless a control operation or a `stop()` registered as waiting
  (`details::WaiterCount` in `src/waiter_count.hpp`, `tests/rt_syscall_tests.cpp`),
  a writer is blocked on the write mutex in `scopedWrite()` or a guard (releasing the
  priority-inheriting mutex is then a `FUTEX_UNLOCK_PI` call), or a sink's worker
  sleeps: that wake comes after the write mutex is released
  (`SinkWorker::Push::wake_owed`). `WriteMutex::try_lock()`, `unlock()` and
  `tryLockWithSpin()` are `noexcept`. The thread-local transaction chain, which a
  snapshot reads after a failed `try_lock()` (`inTransactionOnThisThread()`), is
  initial-exec TLS in `src/channel.cpp`, so no access allocates, also when the library
  is loaded with `dlopen()` (`tests/dlopen_tls_tests.cpp`). The `WriteTransaction`
  constructor and destructor and `inTransactionOnThisThread()` are defined there too:
  keep them out of line (inline code would put the variable in every consumer as
  dynamic TLS), and keep it the library's only thread-local: a `dlopen()`ed library
  takes its initial-exec TLS from glibc's small static TLS surplus.
- The real-time API (the "yes" rows of the Threading table in `docs/llm_user_manual.md`
  and `NsecSinceEpoch()`) is `noexcept` and `DATA_TAMER_NONBLOCKING` (`details/abi.hpp`:
  `[[clang::nonblocking]]` on Clang 20 or later) on the declaration and the out-of-line
  definition (`tryTakeSnapshot()` catches what a serializer throws). Clang infers the
  attribute for the scalar `LoggedValue::set()`/`get()`. A new real-time function gets the macro, a call in
  `tests/compile_fail/nonblocking_caller.cpp` and one in `cycle()` of
  `tests/rtsan_tests.cpp`. The futex calls the bullet above allows run inside
  `details::AllowedRealTimeCall` (`src/realtime_sanitizer.hpp`), and nothing else does.
- `onSchema()`/`onSnapshot()`/`onStop()`/`onStart()` are serialized by the SinkWorker,
  and control operations (registration, sinks, `startLogging()`) wait for them. Never call a
  control operation from a serializer; inside `scopedWrite()` they are safe. Lock order:
  write mutex, then `start_mutex`, then `control_mutex`, then the snapshot epoch; the
  snapshot path takes the write mutex before the epoch. A control operation that would
  wait for a worker, called on the thread running that worker's callback, throws
  `std::logic_error` there and changes nothing (the list is in the `DataSink` doc):
  `SinkWorker::throwIfDelivering()`, over `throwIfDeliverer()` in `src/data_sink.cpp`,
  compares the worker's deliverer id, no thread-local. A new wait for a worker gets the
  same check. Cycles through other workers are not detected.
  `stop()` runs `onStop()` once per stop, after the last delivery, and `start()` after a
  stop runs `onStart()`: a sink that needs to finish (close, flush, dump) or reopen does
  it there.
- `ChannelSharedState` (the write mutex and the series flags shared by a channel and its
  LoggedValues) is library-private, defined in `src/shared_state.hpp`. Consumers see it
  only as an incomplete type: `LoggedValue` holds a `shared_ptr` to it and reaches it
  through exported functions defined in `src/channel.cpp`, the `WriteTransaction`
  constructor and destructor and `details::setSeriesEnabled()`/`isSeriesEnabled()`
  (`logged_value.hpp`). Keep it so: no public header includes `src/`, and its layout,
  the flag word, `AtomicWordTable` and `WriteMutex` stay free to change in a 2.x minor
  release. Because consumers compile no code of the transaction chain, the chain is one
  per process, also with consumers built with `-fvisibility=hidden`;
  `tests/visibility_tests.cpp` checks it in shared builds.
- A registration changes the channel in one step, in `registerValueWithTypes()`:
  everything that can throw runs first and `addSeries()` is the last change; the
  templates only collect custom types (`discoverTypes()`, which also checks that a
  type name belongs to one C++ type). `registerValueWithTypes()` refuses a type name
  that a TypeDefinition type and a `registerCustomValue()` serializer would share: a
  TypeDefinition registration passes the serializer that `TypesRegistry::find()`
  returns for the name. `registration_fault_tests.cpp` fails each
  allocation in turn (`AllocCounter::FailNth`). `poolExhausted()` relies on the pool being created before
  `logging_started` and never replaced.
- A change to the schema text, the payload encoding, the schema hash or the MCAP and ROS
  message layout is a format revision. Update `docs/wire_format.md`, regenerate the
  vectors, update `python/data_tamer_parser.py` (the reference walker, then the compiled
  plans until `test_fast_path.py` passes), `data_tamer_parser.hpp` and the TypeScript
  decoder in `data_tamer_foxglove/src/`, and bump
  `SCHEMA_VERSION` in `types.hpp` (and in `data_tamer_parser.hpp` and the Python
  decoder) when the text changes, the line format or YAML. The
  `WireFormat.*`, Python and TypeScript tests fail until all four decoders agree.
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
  `fwd_header_*` targets check it). `data_tamer.hpp` is the umbrella header: it
  includes `channel.hpp` (and through it `data_sink.hpp`), as 1.x did. Keep heavy
  includes out of the other public headers.
- MCAP is updated by changing the URL and `URL_HASH` of `FetchContent_Declare` in
  `CMakeLists.txt` together with the conan requirement; after the 2.0.0 tag that is an ABI
  change (see "Versioning and ABI policy").
- The library's namespace is `DataTamer2`, named after the major version. `SerializeMe`
  and `DataTamerParser` keep their 1.x names and open `inline namespace v2` inside
  every block (`contrib/SerializeMe.hpp` has two), written out without a macro: the
  standalone parser can't include a library header. So no data_tamer symbol of 2.x has
  the mangled name of a 1.x one, and under ROS 2, where both use `mcap_vendor`, 1.x and
  2.x load into one process (two plugins built against different majors). Outside ROS 2
  both export their bundled `mcap::`. `ABI.SerializeMeAndParserAreInTheInlineNamespaceV2`
  checks the inline ones: add an entity for a new block.

## Versioning and ABI policy

One version number covers the C++ library, the ROS packages, Python and TypeScript
decoders and Foxglove extension (2.0.0 until the 2.0.0 tag). It lives in:

- `data_tamer_cpp/CMakeLists.txt`, `project(... VERSION x.y.z)`. The shared library gets
  `VERSION x.y.z` and `SOVERSION x` (the major), so it installs as
  `libdata_tamer.so.x.y.z` with the symlinks `libdata_tamer.so.x` (the SONAME) and
  `libdata_tamer.so` (under ROS `libdata_tamer_v2.so.x.y.z`, `OUTPUT_NAME`). The compile
  definition `DATA_TAMER_VERSION` and the CMake package version files (`data_tamer`,
  `SameMajorVersion`, also the ROS shim in `share/data_tamer/cmake`) derive from it;
- `data_tamer_cpp/package.xml` and `data_tamer_msgs/package.xml`, `<version>`;
- `data_tamer_cpp/conanfile.py`, `version`;
- `python/data_tamer_parser.py`, `__version__`;
- `data_tamer_foxglove/package.json`, `version` (keep `package-lock.json` consistent);
- `data_tamer_foxglove/extension/package.json`, `version` (2.0.0; keep its lockfile consistent);
- `data_tamer_cpp/CHANGELOG.rst` and `data_tamer_msgs/CHANGELOG.rst`: the head section
  of both is "Unreleased" until the release commit renames it to `x.y.z (date)` (bloom
  makes each package's `debian/changelog` from its own; "* No changes" when empty).

`tools/check_versions.py` compares them and runs as the ctest case `version_consistency`.
Change them together.

To cut a release:

1. In one commit on `main`, rename the head section "Unreleased" of both CHANGELOGs to
   `x.y.z (date)` and set the version in every file above (a first 2.0.0 release keeps
   them, since the tree already says 2.0.0). `python3 tools/check_versions.py` must pass.
2. Tag that commit `x.y.z` (no `v` prefix) and push the tag.

The tag starts two workflows: `python.yml` publishes the Python package to PyPI, and
`release.yml` fails unless the tag equals the `package.xml` version and the CHANGELOG has a
section for it, then creates the GitHub release named after the tag, with that section
as its notes (`tools/release_notes.py --version x.y.z` prints it locally). Running
`release.yml` by hand (workflow_dispatch) is a dry run on the chosen branch: the same
checks, notes and extension tests/packaging, no release. Releases include the
Foxglove `.foxe` archive.

The SOVERSION is the ABI generation. Within one major version a newer
`libdata_tamer.so` must keep working with consumers built against any older header of
that major. That is frozen inside 2.x:

- the exported symbols (name, signature, mangling), including the private back-end
  functions that inline templates call (`LogChannel::registerValueImpl`,
  `registerValueWithTypes`, `typeRegistry`, `controlMutex`, `checkValueName`,
  `schemaFrozen`, `hasCustomType`, `addCustomType`, `sharedState`, `sinkDropped` (the
  inline `LogChannel::stats()` calls it), `TypesRegistry::findOrCreate` and `replace`,
  the `WriteTransaction` constructor and destructor, which `scopedWrite()`, the guards
  and non-scalar `LoggedValue`s call, and `details::setSeriesEnabled` and
  `isSeriesEnabled`, which `LoggedValue::setEnabled()` and `isEnabled()` call) and the
  lock contract between them.
  Nothing in the tree calls `addCustomType` any more: it stays for binaries built
  against older headers, whose `registerValue` templates call it;
- the size, alignment and member offsets of every type in `tests/abi_tests.cpp`:
  by-value structs (`ChannelDefaults`, `MCAPRingOptions`, `ROS2PublisherOptions`,
  `Snapshot`, `Schema`, ...), the classes whose inline code runs in consumers
  (`LoggedValue<T>`, `ValuePtr`, `WriteTransaction`, `ConstPtr`, `MutablePtr`) and
  the Pimpl classes (their pointer members). Pinning `Snapshot`, `MCAPRingDump` and
  `Schema` completely is deliberate, although a field appended to them would be safe
  for readers. The exact byte values are for x86_64 only (other platforms lay out the
  standard library types differently). A new pin with a number needs the number
  measured on x86_64, and a relation if it can differ elsewhere (a mutex: 40 bytes on
  x86_64, 48 on aarch64). The `Stats` structs are not on this list: they
  are built inline from exported getters (see Invariants), so their getters are frozen
  instead;
- the vtables of `DataSink` and `CustomSerializer` (see Invariants). abidiff cannot see
  `CustomSerializer`'s in the library, which is why the check also builds a probe;
- the inline protocols: the `registerValue` templates, the payload encoding and schema
  text (any wire revision, see Invariants). The transaction chain and the series flag
  word are not among them: their code is in the library, and only the size of
  `WriteTransaction` reaches consumers;
- the MCAP version that CMake downloads (2.1.3). libdata_tamer compiles the MCAP
  implementation and exports its `mcap::` symbols, and `sinks/mcap_encoding.hpp` hands
  consumers MCAP types, so pinning another MCAP release after the 2.0.0 tag is a
  break. Under ROS 2 MCAP's ABI is `mcap_vendor`'s; a build with
  `DATA_TAMER_USE_SYSTEM_MCAP` (conan) has the ABI of the MCAP package it found, which
  its packager keeps fixed within 2.x. No other library's symbols are exported:
  zstd and lz4 are linked PRIVATE, shared first, and `--exclude-libs,ALL` hides the
  symbols of any static archive linked into the shared library.

Adding a function, an overload, a class or an appended enumerator is compatible. Header
defaults (member initializers, default arguments) take effect only in rebuilt
consumers; defaults inside a Pimpl take effect for old binaries too. Say which in the
CHANGELOG. Adding `DATA_TAMER_NONBLOCKING` or `noexcept` to a non-template function
changes no symbol (Itanium mangling has neither; `nm -D` is identical); removing the
attribute is a source break for callers built with `-Wfunction-effects -Werror`.

Changing anything frozen is a break: bump the major version in all the files above
(so `SOVERSION` changes), rename the namespaces (`DataTamer2` to `DataTamer3`, the inline
`v2` to `v3`) and record it in the CHANGELOG. Put new state behind a Pimpl
instead of growing a pinned layout. Options structs passed by value are not size-tagged,
so adding a field to one is a break too.

Before the first 2.0.0 tag, a layout change to a pinned struct or class is allowed in
the PR that owns it, with the pins in `abi_tests.cpp` updated in the same commit. The
vtables of `DataSink` and `CustomSerializer` and the inline `Stats` rule (see
Invariants) are frozen already. Do not change the library's default symbol visibility
or mark classes `final` outside the planned visibility change.

### Checking the ABI locally

CI (`.github/workflows/abi.yml`, pull requests and pushes to `main`) builds the library
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
(the previous commit on a push to `main`). Locally:

```bash
sudo apt install abigail-tools libzstd-dev liblz4-dev     # abidiff
tools/abi_check.sh                         # automatic baseline, as in CI
tools/abi_check.sh --baseline-ref 2.0.0    # any tag, branch or commit
tools/abi_check.sh --baseline-dir /path/to/exported/tree
```

The script builds both sides (plain CMake, shared, Debug, no ROS) in a temporary
directory and prints the abidiff report. A removed function counts only if the baseline
library defined it strongly: the weak copies of `std::` and other inline functions
that the library exports are not API. Changes of the `std::` and `__gnu_cxx::` copies
(a Pimpl built with `make_shared` that grew) are suppressed by
`tools/abi_suppressions.ini`. A change to `tools/abi_probe.cpp` that uses new
API still gets its consumer-side comparison, with the baseline's own probe. The ROS 2
sink is not covered: its ABI also follows the rclcpp release, and its pins are in
`abi_tests.cpp`. Before 2.0.0, a layout change that the PR owns is accepted with
`--allow-break` locally and the `abi-break` label on the pull request (never for the
frozen vtables); once a 2.x tag exists the label no longer works and only a major
version bump passes. The check also runs on pushes to `main`, against the previous
commit; there it reads the label from the pull request that made the commit, since a push
event carries no labels.

## Conventions

- Format with the repository `.clang-format` (Google based, 90 columns, braces on their
  own line). Run `pre-commit run --files <changed files>`: it pins clang-format 23.1.1,
  and the distribution clang-format 18 formats some files differently. Avoid
  `--all-files`, which rewrites files unrelated to your change.
- Tests: gtest, `TEST(Suite, BehaviourInCamelCase)`, new files listed in
  `DATATAMER_TEST_SOURCES` (`tests/CMakeLists.txt`). Helpers live in
  `tests/test_sinks.hpp` (`Attached<T>`, `channelWith`, `acceptedUntilExhausted`) and
  `tests/mcap_test_utils.hpp` (`tempPath`, `ScratchDir`, `countMessages`,
  `summaryMessageCount`, `logTimes`). Wait for delivery with `SinkWorker::drain()` or
  `Delivery::Manual`, never with sleeps. A test that could hang runs in a death-test
  child with a watchdog (`tests/hang_watchdog.hpp`); `tests/observed_thread.hpp` waits
  until a thread blocks and `tests/gate.hpp` parks one. Out-of-bounds tests read from
  or write into `tests/guarded_buffer.hpp`, which ends at an inaccessible page.
- Code that must not compile gets a probe in `tests/compile_fail/` and a
  `data_tamer_compile_fail_test()` call in `tests/CMakeLists.txt`: the probe compiles as
  written (the `_control` target), a `DT_` macro breaks it, and the test passes when the
  build output matches the required `REGEX`, the diagnostic the user gets.
  `parser_header_warnings`
  compiles the standalone parser as strict C++17 with `-Werror`.
- Commits: short imperative subject, optionally prefixed by the component
  (`MCAPSink: roll over into new files by default`). The body says why, and what was
  tested.
