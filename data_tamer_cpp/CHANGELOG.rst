^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
Changelog for package data_tamer
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Unreleased
----------
* ROS 2: export the ``Threads`` dependency, so that ``find_package(data_tamer_cpp)``
  works from a fresh CMake cache (``Threads::Threads`` is in the public link
  interface).
* ``ROS2PublisherSink`` can aggregate snapshots: pass ``ROS2PublisherOptions``
  with ``aggregate = true`` to publish ``data_tamer_msgs/SnapshotBatch`` on
  ``<prefix>/data_batch`` instead of one ``Snapshot`` per sample on
  ``<prefix>/data``. A batch is sent when it reaches ``max_batch_size``
  snapshots, when a snapshot arrives ``max_batch_delay`` after the first one of
  the batch, on ``flush()`` and when the sink is destroyed. ``embed_schemas``
  (default on) puts the schemas of the batch's snapshots in the message, so it
  decodes without the ``schemas`` topic. Schema texts are now serialized once,
  in ``onSchema()``, instead of on every republish.
* ``ROS2PublisherSink`` QoS (2.0 review item 18). The data topic (``data`` or
  ``data_batch``) uses the new ``ROS2PublisherOptions::data_qos``. Its default
  is unchanged, reliable ``KeepAll``, under which a slow or stalled subscriber
  makes the publishing process queue messages without bound; in robot
  processes a bounded history is recommended, e.g.
  ``rclcpp::QoS(rclcpp::KeepLast(100)).reliable()``.
  ``schemas`` is reliable, transient-local ``KeepLast(1)``: every message is the
  complete catalog, so a late subscriber still gets all schemas. The catalog is
  published as soon as the sink learns a schema (``prepare()``, or
  ``addDataSink()`` on a prepared channel) instead of with the next snapshot, so
  call ``prepare()`` explicitly outside the control loop. A failed publication
  does not fail ``prepare()``: it is retried by the next snapshot (counted in
  ``SinkWorker::errors()``) or by ``flush()``, which now publishes a pending
  catalog even without aggregation and throws if that fails.
* Parser helpers to decode the ROS messages without depending on ROS (templates
  on the message type): ``DataTamerParser::SchemaRegistry`` (schemas by hash,
  filled from ``Schemas`` or embedded batch schemas), ``ToSnapshotView()`` and
  ``ForEachSnapshotInBatch()``. Python equivalents in ``data_tamer_parser.py``:
  ``SchemaRegistry``, ``parse_snapshot_msg()`` and ``iter_snapshot_batch()``.
* YAML schema rendering (wire format version 6, section 2.1): ``ToYaml(schema)``
  writes the same schema as YAML, nesting fields whose names share a
  ``/``-separated prefix, which is shorter for path-like names (the hash is
  unchanged). ``RenderSchema(schema, SchemaFormat)`` picks either rendering;
  opt in for ROS with ``ROS2PublisherOptions::schema_format =
  SchemaFormat::Yaml``.
  ``BuildSchemaFromText()`` and the Python ``parse_schema()`` detect and read
  both renderings without dependencies, and verify a YAML schema's hash via
  ``ToText()`` / ``to_text()``. The parser's ``Schema`` gains
  ``custom_schemas`` (opaque types, filled from YAML).
* Examples: ``ros2_publisher`` takes ``--aggregate`` and ``--yaml``;
  ``python/ros2_subscriber.py`` decodes ``Snapshot`` and ``SnapshotBatch``
  topics with the Python decoder.
* The Python decoder is a package, ``data-tamer-parser`` (``python/pyproject.toml``,
  still standard library only, importable as ``data_tamer_parser``; version
  ``data_tamer_parser.__version__``): ``pip install ./python``. The ``python``
  workflow tests and builds it and publishes it to PyPI on ``X.Y.Z`` tags; it
  is versioned in lockstep with the library (release tag == ``package.xml``
  version == ``data_tamer_parser.__version__``). New
  ``Schema.field_names()`` lists the flattened names from the schema alone
  (dynamic vector elements as the placeholder ``vec[]``; bounded by
  ``MAX_SCHEMA_DEPTH`` and ``MAX_FIELD_NAMES``), and ``iter_mcap(path)`` yields
  ``(timestamp, topic, values)`` for an MCAP file, with the optional ``mcap``
  package (``data-tamer-parser[mcap]``).
* New customization point ``DataTamer::TypeDefinitionTrait<T, Enable = void>``
  (#94): describe a type with a ``static define(T&, AddField&)`` specialization
  instead of a ``TypeDefinition`` overload in the type's own namespace, so
  third-party types (Eigen, ...) no longer require reopening their namespace.
  Partial specializations (including ``enable_if`` on the second parameter) are
  supported. An optional ``static std::string name()`` builds the type name at
  runtime (once per type, per shared library), e.g. for class templates; a
  ``define()`` returning ``std::string`` is cached the same way. When both a
  trait and an ADL ``TypeDefinition`` exist, the trait wins; a specialization
  with an unusable ``define()`` is a compile error. Existing ``TypeDefinition``
  overloads keep working. ``CustomTypeName<T>::get()`` is no longer
  ``constexpr``. ``SerializeMe::DeserializeFromBuffer`` now compiles for custom
  types (it passed const field pointers and could not write the fields).
* Value names (#97): new header-only ``DataTamer::JoinNames(parts...)``
  (``data_tamer/names.hpp``, included by ``channel.hpp``) joins components with
  a single ``/`` and drops empty ones, so namespaces may carry trailing slashes:
  ``JoinNames("/loco/", "LF/", "x") == "loco/LF/x"``. Names with empty
  ``/``-separated components (e.g. ``"/loco//torso/x"``) are still accepted,
  but PlotJuggler shows them as empty path elements; the new
  ``DataTamer::IsCanonicalName(name)`` (non-empty, no spaces, no empty
  components) is an opt-in check to assert on. Registration rules are
  unchanged (only spaces in value names are rejected). Registration errors now
  name the channel and the value, e.g. ``channel 'controller/walk': value
  'loco/LF/x' registered twice (unregister() it first)``; the no-spaces check
  runs before any custom type is discovered, so a rejected name leaves the
  channel unchanged.
* MCAP rollover (#98): the default is unchanged: when ``setMaxTimeBeforeReset``
  expires (600 s by default), ``MCAPSink`` truncates and restarts the same
  file, DISCARDING everything recorded before the reset. To keep it, call
  ``setCreateNewFileOnReset(true)`` (continue in a new numbered file) or
  ``setMaxTimeBeforeReset(std::chrono::seconds(0))`` (never reset). With
  rollover enabled, numbered names that already exist (e.g. from a previous run
  with the same path) are now skipped instead of overwritten, and the counter
  advances only once the new file is open. The counter is inserted before the
  extension, now the trailing run of alphabetic dot-segments, so multi-part
  extensions survive and dotted stems stay whole: ``run.tamer.mcap`` rolls over
  to ``run_1.tamer.mcap`` (it was ``run.tamer_1.mcap``), ``log_2026.10.05.mcap``
  to ``log_2026.10.05_1.mcap``.
* MCAP messages carry a per-channel ``sequence`` number (1, 2, 3, ... per MCAP
  channel and file) instead of always 1, so readers can detect gaps (#98).
* New ``data_tamer/sinks/mcap_encoding.hpp`` (#96): inline helpers
  ``mcap_encoding::AddChannel()``, ``WriteSnapshot()`` (for a ``Snapshot``),
  ``WriteMessage()`` (from a timestamp and mask and payload spans, e.g.
  for stored data) and ``EncodeMessageBody()`` that write the MCAP records of
  ``docs/wire_format.md``. ``MCAPSink`` uses them. The caller provides the
  sequence number and a reusable scratch buffer (no allocation once it is large
  enough). They need ``data_tamer::data_tamer`` (and ``mcap_vendor::mcap`` under
  ROS 2); outside ROS 2 the bundled MCAP headers are now installed (under
  ``include/data_tamer_mcap``) and exported with data_tamer through the
  header-only ``data_tamer::mcap_headers`` target, so a shared install exports
  no zstd/lz4 library paths; libdata_tamer already contains the MCAP
  implementation (do not define ``MCAP_IMPLEMENTATION``). A shared build no
  longer installs ``libmcap_lib.a``. ``SerializeMe::Span<const T>`` converts from a
  ``const std::vector<T>&``. The ``DataSink`` documentation now explains that a
  slow ``onSnapshot()`` exhausts the channel's shared snapshot pool for every sink.
* **Breaking, channel API cleanup** (2.0 review items 20-24, 33):

  - ``RegistrationID`` is an opaque handle (no public ``first_index`` /
    ``fields_count``, no ``operator+=``). It denotes one registration: after
    ``unregister()`` and a new registration of the same name the old handle is
    stale, and ``LogChannel::setEnabled`` / ``unregister`` throw
    ``std::invalid_argument`` instead of acting on the replacement. New
    ``LogChannel::isEnabled(id)`` and noexcept ``trySetEnabled(id, enable)``.
    The per-series flags live in an append-only table whose reads are safe to
    race with registration (they were a ``std::deque`` before).
    Re-registering the same name more than 16 million times throws.
  - ``LoggedValue::set(value)`` only stores; the ``auto_enable`` parameter is
    gone. Re-enable with ``setEnabled(true)``.
  - ``LoggedValue::getMutablePtr()`` / ``getConstPtr()`` exist only for
    non-scalar values (scalars: ``set()`` / ``get()``), hold the channel's write
    transaction instead of the raw mutex, so they nest inside ``scopedWrite()``,
    and are no longer movable (like ``std::lock_guard``). Removed:
    ``getLockedPtr()``, the ``mutex()`` accessors, ``AtomicProxy`` /
    ``AtomicConstProxy``.
  - Removed ``LogChannel::writeMutex()``, ``sharedState()`` and the global
    ``Mutex`` alias. ``scopedWrite()`` returns ``DataTamer::WriteTransaction``.
  - Parser: ``BuildSchemaFromText`` is the entry point; the misspelled
    ``BuilSchemaFromText`` remains as a deprecated alias for one release.
* **Breaking, snapshots**: ``takeSnapshot()`` returns a ``[[nodiscard]]``
  ``SnapshotResult`` instead of ``bool`` (``ok``, ``partial``, ``rejected``,
  ``no_sinks``, ``not_prepared``, ``pool_exhausted``, ``oversize``, ``blocked``). New
  ``tryTakeSnapshot()`` for real-time producers: never blocks on the write mutex
  and never grows a slot; it requires ``prepare()``. ``setStrictMode()`` is
  removed (it was ``tryTakeSnapshot()``'s no-growth behaviour); the
  ``droppedOversize()`` counter stays.
* New ``LogChannel::prepare()`` / ``isPrepared()``: freeze the schema, allocate
  the pool and announce the schema to the sinks explicitly. A failed
  ``prepare()`` leaves the channel exactly as it was (schema open, settable
  capacities) so it can be retried; if the schema changes afterwards, sinks that
  already heard it are announced again. ``takeSnapshot()`` without sinks now
  returns ``no_sinks`` without freezing anything. Sink ``onSchema`` callbacks run
  with the channel control mutex released, so a sink may query the channel.
* **Breaking, sinks**: ``DataSinkBase`` is replaced by composition. A sink
  implements ``DataSink`` (``onSchema(const Schema&)``, ``onSnapshot(const
  SnapshotRef&)``; throw to report a failure) and is owned by a ``SinkWorker``,
  which holds the queue and the delivery thread and is what
  ``LogChannel::addDataSink`` takes. The worker is always stopped before the
  sink is destroyed, and its destructor delivers what is still queued, so the
  old ``stopThread()``-in-every-destructor rule and the four lifecycle calls
  (``stopThread``, ``stopAcceptingSnapshots``, ``processQueuedSnapshots``,
  ``startAcceptingSnapshots``) are gone; use ``SinkWorker::stop()``,
  ``start()`` and ``drain()``. ``SinkWorker::Delivery::Manual`` runs without a
  thread for applications that deliver from their own loop.
  ``MCAPSink::create(path)``, ``ROS2PublisherSink::create(node, prefix)`` and
  ``SinkWorker::create<T>(...)`` return a ready-to-attach worker;
  ``worker->as<MCAPSink>()`` reaches the sink's own methods.
  ``MCAPSink::finishQueueAndStop()`` is ``worker->stop()`` followed by
  ``stopRecording()``; ``restartRecording()`` no longer reopens admission, call
  ``worker->start()`` after a stop. A failed MCAP write now throws from the
  callback and is reported by ``SinkWorker::errors()`` / ``lastError()``.
* **Breaking, snapshots**: ``Snapshot`` no longer carries ``channel_name`` (the
  schema hash identifies the channel; ``Schema::channel_name`` has the name), so
  it is fully owning. ``SnapshotRef`` is now declared in ``data_sink.hpp`` as an
  opaque move-only handle: keep a snapshot past the callback with
  ``ref.clone()``; ``DataSinkBase::retainSnapshot()`` and its thread-local
  context are gone.
* Build: the library, tests and benchmarks compile as C++20; the installed
  headers remain C++17 (consumers need ``cxx_std_17``), enforced by a test
  target that compiles every core public header as strict C++17 and by building
  the examples as C++17. The ROS 2 sink header follows rclcpp's standard, which
  is C++20 on recent distributions. Internally, the reader-quiescence and sink-admission
  barriers block on ``std::atomic::wait`` instead of yield-spinning, and the
  sink worker is a ``std::jthread``.
* Build: CMake 3.22 is now required. The build is target-based: MCAP, rclcpp,
  rclcpp_lifecycle and the message package are linked as imported targets
  (``mcap_vendor::mcap``, ``rclcpp::rclcpp``, ...) instead of ``*_INCLUDE_DIRS``
  and ``*_LIBRARIES`` variables, the library links ``Threads::Threads`` and
  exports a ``data_tamer::data_tamer`` alias in-tree, ``DATA_TAMER_VERSION``
  reports the library's own version when built as a subproject, and the
  installed ``data_tamerConfig.cmake`` finds its dependencies.
* Robustness fixes from the 2.0 review: the header-only parser and the Python
  decoder check bounds before every read, reject masks shorter than the schema,
  oversized dynamic counts and cyclic custom types, match type names exactly and
  validate fixed-array extents; ``SchemaHash``/``schema_hash`` fields are
  ``uint64_t`` end to end; ``TypeField::operator!=`` is defined. Library: the
  fixed-size accumulator for arrays was uninitialized; custom types may now
  contain numeric ``std::array``/``std::vector`` members; a failed registration
  leaves the channel unchanged; null custom serializers are rejected; fixed
  array extents outside 1..65535 fail to compile; the active mask is refreshed
  under the write mutex so values enabled inside one transaction are captured
  together; schema text is locale independent. ``MCAPSink`` reports write
  failures from ``storeSnapshot()``, opens a new file before closing the old one
  on restart, and synchronizes its setters with the worker. The ROS 2 sink
  retries schema publication after a failure. ``ChannelsRegistry::clear()``
  destroys channels outside its lock; ``addDefaultSink(nullptr)`` throws.
* Schema version 5: the schema hash is FNV-1a 64 of the schema text (minus its
  own hash line), so it is identical on every platform, verifiable by any
  decoder and covers custom type bodies. Readers accept version 4 files through
  their declared hash; parsers older than this release reject version 5 files
  with "Wrong SCHEMA_VERSION". In the library ``AddFieldToHash`` is replaced by
  ``SchemaTextHash``/``ComputeSchemaHash``; the header-only parser keeps it
  for version 4 texts.
* Real-time front end, steps 0–8: scalar ``LoggedValue`` values use lock-free
  atomics; ``set()``/``get()`` are wait-free. Non-scalar updates and snapshot
  serialization share a priority-inheriting mutex. ``LogChannel::scopedWrite()``
  groups updates into one transaction and supports nested non-scalar ``set()``/``get()``.
  Lone scalar updates do not promise consistency across values.
* ``takeSnapshot()`` now freezes schema and capacity state on its first attempt,
  then serializes directly into a 64-slot channel pool. Its fixed-size warmed
  library path takes no structure mutex and performs no heap allocation; user
  serializers and non-strict payload growth may still allocate or throw.
  Sequentially consistent sink, liveness, mask-dirty and reader-epoch
  publication makes concurrent value destruction and sink removal safe.
* Added pre-freeze ``setPayloadCapacity()`` and ``setPoolCapacity()`` plus
  runtime ``setStrictMode()``. Initial per-slot reservation is at least the
  maximum of the hint, twice the initial payload and 256 bytes. Strict mode
  drops an oversize snapshot against the acquired slot's actual capacity, so a
  later input cannot grow that slot; non-strict mode grows that slot and retains
  the new capacity.
* Added ``writeLockContended()``, ``writeLockWaitMaxNs()`` and ``stats()``.
  Contention counts acquisitions that block after spinning; maximum wait measures
  blocking acquisition time, excluding serialization.
* API: ``Mutex`` now aliases exclusive ``WriteMutex`` (no ``lock_shared()``);
  ``LoggedValue::get()`` is const; scalar ``getMutablePtr()``/``getConstPtr()``
  are deprecated in favor of ``set()``/``get()``. Proxy ``mutex()`` is deprecated
  and proxy boolean conversion is explicit. ``LoggedValue`` is no longer movable.
  ``DummySink`` exposes synchronized accessors instead of public members.
* ``MCAPSink`` and ``ROS2PublisherSink`` store private state behind a Pimpl.
  This release changes their ABI; downstream binaries must be rebuilt.
* Sink delivery now shares snapshots through a 64-slot channel pool and a
  preallocated blocking queue. ``DataSinkBase(size_t queue_capacity = 1024)``
  treats capacity as a block-rounded minimum shared by all channel producers;
  ``DummySink`` and ``MCAPSink`` forward the same final defaulted argument.
  ``DataSinkBase::pushSnapshot`` was removed.
* Added ``LogChannel::poolExhausted()``, per-attachment
  ``droppedSnapshots(sink)``, ``Stats::pool_exhausted``, and
  ``DataSinkBase::storeErrors()``, plus payload reallocation and oversize-drop
  counters. ``Stats`` fields are plain ``uint64_t`` point-in-time snapshots of
  relaxed atomic counters; ``droppedSnapshots(sink)`` takes the control mutex.
  ``DataSinkBase::storeErrors()`` records thrown queued callbacks; a callback
  returning ``false`` is not an exception.
* A channel supports eight attached sinks. Same-name compatible
  re-registration reuses its schema slot after freeze; registration IDs identify
  slots rather than generations. Control operations must run outside writer
  guards and serializer callbacks.
* Derived sinks must call ``stopThread()`` before destroying callback state.
  Queued callbacks are serialized with manual draining, exceptions do not stop
  delivery, and protected callback-only ``retainSnapshot()`` can keep pooled
  payload, mask and channel-name storage alive without changing the
  ``storeSnapshot(const Snapshot&)`` virtual API.
* Closing sink admission waits for already admitted enqueues and drains all
  accepted references. ``MCAPSink::finishQueueAndStop()`` no longer polls or
  sleeps; explicit restart clears forced-stop state and reopens admission,
  while automatic rollover preserves an existing closure.
* Explicit producer tokens preserve callback order within one continuous
  channel/sink attachment. Removing and re-attaching a sink replaces the token;
  newer work may then run before older queued records from the prior attachment,
  with no ordering guarantee across that boundary. A sink shared by multiple
  channels also does not promise global timestamp order; readers that require a
  merged timeline must sort or merge it.
* Build: debug/release/asan/tsan presets, sanitizer CI, allocation-counting
  benchmarks and the ``rt_latency`` harness, including standalone mutex/pool
  measurements and validated CLI inputs. Conan's benchmark option exports and
  builds the benchmark sources. Vendored MCAP builds with GCC 15.

1.0.4 (2026-07-26)
------------------
* Merge pull request `#68 <https://github.com/PickNikRobotics/data_tamer/issues/68>`_ from coderjake91/feature/update-ROS2PublisherSink-to-use-NodeInterfaces
  Update ROS 2 publisher sink to use NodeInterfaces
* Merge pull request `#65 <https://github.com/PickNikRobotics/data_tamer/issues/65>`_ from Basiljamal1/main
  Add thread-safe methods to add and remove data sinks in LogChannel
* modify logging_started toggle to avoid possible (benign) race condition
* clarify addDataSink logic with comment and simplify code
* doxygen name fix
* test: add and improve tests for adding and removing data sinks in LogChannel
* Merge pull request `#62 <https://github.com/PickNikRobotics/data_tamer/issues/62>`_ from PickNikRobotics/finish_queue_before_stop
  add ability to finish queue then stop recording
* fix: Updated header file for channel.hpp
* Add thread-safe methods to add and remove data sinks in LogChannel
* Merge pull request `#64 <https://github.com/PickNikRobotics/data_tamer/issues/64>`_ from Shibodd/cmake_benchmarks_option
  CMakeLists: add DATA_TAMER_BUILD_BENCHMARKS option
* CMakeLists: add DATA_TAMER_BUILD_BENCHMARKS option
* add ability to finish queue then stop recording
* Inline operator== function to prevent multiple includes (`#60 <https://github.com/PickNikRobotics/data_tamer/issues/60>`_)
  Co-authored-by: jlack <jlack@nauticusrobotics.com>
* Fix compilation on Windows by exporting all symbols with CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS (`#58 <https://github.com/PickNikRobotics/data_tamer/issues/58>`_)
* Merge pull request `#40 <https://github.com/PickNikRobotics/data_tamer/issues/40>`_ from damien-robotsix/custom_container_handling
  fix: handling of custom containers that has a TypeDefinition
* add trait 'tests'
* fix: ub in SerializeIntoBuffer
* fix: handling of custom containers that has a TypeDefinition
* Contributors: Basil Jamal, Damien SIX, Henry Moore, Jacob Frazer, Silvio Traversaro, Unimore, jlack1987

1.0.3 (2025-05-23)
-----------
* Remove ament_target_dependencies usage

1.0.2 (2025-05-12)
-----------
* Merge pull request `#52 <https://github.com/PickNikRobotics/data_tamer/issues/52>`_ from PickNikRobotics/fix_build_farm_3
  Try installing test deps no matter what
* try installing test deps no matter what
* Merge pull request `#51 <https://github.com/PickNikRobotics/data_tamer/issues/51>`_ from PickNikRobotics/fix_build_farm_2
  ignore build ros argument in test CMake
* ignore build ros argument in test
* Merge pull request `#50 <https://github.com/PickNikRobotics/data_tamer/issues/50>`_ from PickNikRobotics/fix_build_farm
  Fix ament GTest usage
* use ament add gtest
* Merge pull request `#49 <https://github.com/PickNikRobotics/data_tamer/issues/49>`_ from PickNikRobotics/fix_gtest_link
  Get gtest from vendor on ROS
* add myself to maintainer list, add package xml scheme
* get gtest from vendor on ROS
* Contributors: Henry Moore

1.0.1 (2025-03-03)
------------------
* force the size of BasicType ot be 1 byte (`#47 <https://github.com/PickNikRobotics/data_tamer/issues/47>`_)
  * force the size of BasicType ot be 1 byte
  This fix the issue when the log is generated on two different computers
  using different compilers (example Linux / QNX)
  * fix
* add file reset capabilities (`#37 <https://github.com/PickNikRobotics/data_tamer/issues/37>`_)
* Enable users to build without ROS (`#36 <https://github.com/PickNikRobotics/data_tamer/issues/36>`_)
  * make building with ROS an option
  * clarify warnings, allow for default-building without ROS
  * switch name of ros build flag
  * add build ros argument for examples
  * fix incorrect message
* Merge pull request `#32 <https://github.com/PickNikRobotics/data_tamer/issues/32>`_ from PickNikRobotics/default_increment_filename
  Allow endless recording
* remove unused variable
* allow for unlimited recording
* use a new mutex wrapper API (`#27 <https://github.com/PickNikRobotics/data_tamer/issues/27>`_)
  * use a new mutx wrapper API
  * fix compilation
  * change docstrings to reference non-deprecated function
  * add now-missing includes
  ---------
  Co-authored-by: Henry Moore <henry.moore@picknik.ai>
* Locked ptr test, documentation, and example (`#24 <https://github.com/PickNikRobotics/data_tamer/issues/24>`_)
  * add locked ptr usage to example
  * update comment relating to locked ptr
  * remove unused headers
  * add test for locked ptr and non-blocking method
  * remove unsafe lockedPtr get function
* Merge pull request `#23 <https://github.com/PickNikRobotics/data_tamer/issues/23>`_ from torsoelectronics/main
  Fix compile error
* Fix build error
* Contributors: Daniel Mouritzen, Davide Faconti, Henry Moore

1.0.0 (2024-04-30)
------------------
* Support lifecycle node for ros2 publisher sink (`#17 <https://github.com/PickNikRobotics/data_tamer/issues/17>`_)
  * Support lifecycle node for ros2 publisher sink
  * Remove unused member variable node\_
  * Add template for both constructors
* more efficient locking of LoggedValue<T> and new clang format
* refactoring custom types
* fix compilation with and without conan
* new clang format
* add mcap to 3rdparty
* Contributors: Davide Faconti, Victor Massagué Respall

0.9.4 (2024-02-02)
------------------
* changed the way registerValue throws if you try registering the same address again
* add unit tests to verify that vectors with changing size are OK
* Contributors: Davide Faconti

0.9.3 (2024-02-01)
------------------
* add std::hash<DataTamer::RegistrationID>
* fix dead-lock
* Contributors: Davide Faconti

0.9.2 (2024-01-30)
------------------
* fix compilation in ament
* Update CMakeLists.txt. Fix `#11 <https://github.com/facontidavide/data_tamer/issues/11>`_
* Contributors: Davide Faconti

0.9.1 (2024-01-12)
------------------
* add support for enums
* renamed folder to data_tamer_cpp
* Contributors: Davide Faconti

0.8.0 (2023-11-30)
------------------
* API change related to CustomSerializers
* Contributors: Davide Faconti

0.7.0 (2023-11-28)
------------------
* recursive_mutex and call it a day
* add MCAP option
* add MCAPSink::stopRecording
* add more types to mcap example
* add ChannelsRegistry::clear()
* extended tests
* bug fixes and more tests
* fix warning
* compute fixed size at compilation time
* new wrappying of TypeDefinition
* refactoring type registry
* major refactoring of custom types
* Contributors: Davide Faconti

0.6.0 (2023-11-23)
------------------
@ add back compatibility to data_tamer_parser
* works correctly with plotjuggler
* fix ROS2 compilation
* Contributors: Davide Faconti

0.5.0 (2023-11-22)
------------------
* preliminary custom type support
* Contributors: Davide Faconti

0.4.1 (2023-11-21)
------------------

0.4.0 (2023-11-21)
------------------
* add again channel name to hash
* bug fixes in schema hash and parsing
* add benchmark
* readme update
* added data_tamer_parser with some samples and testing
* add locked reference
* bug fixes and tests
* refactored API to support containers
* Contributors: Davide Faconti

0.3.0 (2023-11-14)
------------------
* add coverage
* fix bug
* add CI
* unit test added
* allow registering again with new pointer
* add docs
* use custom mutex on linux
* adding ros2 example
* ros2 publisher sink
* Contributors: Davide Faconti

0.2.1 (2023-11-13)
------------------
* fix conan
* fix conan
* Contributors: Davide Faconti

0.2.0 (2023-11-13)
------------------
* First release: supports MCAP sink only
* Contributors: Davide Faconti, Henry Moore
