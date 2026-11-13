^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
Changelog for package data_tamer
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Unreleased
----------
* ``MCAPSink`` and ``MCAPRingSink`` keep the page cache of the files they write bounded
  on Linux: every 8 MiB they start the writeback of the new data, wait for the previous
  8 MiB to reach the disk and drop it from the cache, so about 24 MiB of a file is
  cached at most. A 2 GiB dump no longer leaves 2 GiB of dirty pages for the kernel to
  flush at once, which flooded the disk queue and stalled other processes on page
  faults. Closing a file now runs ``fsync()``: ``stop()``, ``stopRecording()`` and a
  rollover return once the file is on disk.
* ``MCAPSink`` reports failed writes. It used ``mcap::FileWriter``, whose ``fwrite()``
  check is an ``assert``, so a full disk produced a truncated file with no error. Now a
  failed write throws from ``onSnapshot()`` (counted in ``SinkWorker::errors()``), and
  a failure while closing the file throws from ``stopRecording()``, or from the
  ``onSnapshot()`` or ``restartRecording()`` that closed it. ``MCAPRingDump`` reports a
  failed ``fsync()`` or ``close()`` too.
* The sink worker threads are named ``dt_sink_worker`` and the ``MCAPRingSink`` writer
  thread ``dt_ring_writer`` (Linux, macOS), for ``top -H``, perf and debuggers.

2.0.0 (2026-10-24)
------------------
First release of the 2.x series, relative to 1.0.4. It breaks source and binary
compatibility: rebuild every binary that links data_tamer, and read ``docs/MIGRATION.md``,
which gives the replacement for each removed API and lists the behaviour changes that
compile without a warning. Readers need the 2.0 parser for files recorded by 2.0; it still
reads 1.x files.

**Breaking changes**

* The namespace is ``DataTamer2`` (was ``DataTamer``), the headers are in
  ``data_tamer_v2/`` (were ``data_tamer/``) and the standalone parser is
  ``data_tamer_v2/data_tamer_parser.hpp`` (was
  ``data_tamer_parser/data_tamer_parser.hpp``). ``SerializeMe`` and ``DataTamerParser``
  keep their names, inside an inline namespace ``v2``.
* ``DataSinkBase`` is replaced by ``DataSink``, whose protected callbacks are
  ``onSchema()``, ``onSnapshot(const SnapshotRef&)`` and the optional ``onStop()`` and
  ``onStart()`` (throw to report a failure), owned by a ``SinkWorker`` that runs the
  delivery thread. ``addChannel()``, ``pushSnapshot()``, ``storeSnapshot()``,
  ``stopThread()``, ``stopAcceptingSnapshots()``, ``processQueuedSnapshots()`` and
  ``startAcceptingSnapshots()`` are gone: use ``SinkWorker::stop()``, ``drain()`` and
  ``start()``. To write data_tamer MCAP files from your own data, use
  ``sinks/mcap_encoding.hpp``.
* Sinks are created with ``MCAPSink::create()``, ``ROS2PublisherSink::create()``,
  ``DummySink::create()`` or ``SinkWorker::create<T>()``, which return a
  ``std::shared_ptr<SinkWorkerOf<T>>``: a ``SinkWorker`` whose ``sink()`` is the ``T``
  (static cast, no ``dynamic_cast``), converting to the ``std::shared_ptr<SinkWorker>``
  that ``addDataSink()`` and ``addDefaultSink()`` take. ``worker->as<T>()`` reaches the
  sink through a plain ``SinkWorker`` (``std::bad_cast`` for the wrong type). Passing
  ``std::make_shared<MCAPSink>(path)``, as in 1.x, to ``addDataSink()``,
  ``removeDataSink()`` or ``addDefaultSink()`` fails with a ``static_assert`` that names
  ``MCAPSink::create()`` and ``SinkWorker::create<T>()``.
  ``LogChannel::addDataSink()`` takes the worker and returns ``bool`` (``false`` if it was
  attached already). ``MCAPSink::finishQueueAndStop()`` is removed (``worker->stop()``),
  ``restartRecording()`` no longer resumes a stopped worker, and ``DummySink`` has
  accessors (``latestSnapshot()``, ``schema(hash)``, ...) instead of public members.
* ``Snapshot`` has no ``channel_name`` (use ``Schema::channel_name``, matched by
  ``schema_hash``), and ``schema_hash`` is ``uint64_t``. ``DataSnapshot`` is removed.
* ``takeSnapshot()`` returns a ``SnapshotResult`` instead of ``bool`` (``accepted(result)``
  replaces the ``bool``).
  ``getActiveFlags()`` is removed: ``isEnabled(id)`` in the application,
  ``Snapshot::active_mask`` in a sink.
* ``LogChannel::writeMutex()`` and the ``Mutex`` alias are removed: another thread writes
  registered variables inside ``scopedWrite()``.
* ``LoggedValue``: scalars are atomic and use ``set()`` and ``get()`` only. Structs and
  vectors keep ``getMutablePtr()`` and ``getConstPtr()``, whose guards are not movable and
  have no ``operator bool`` and no ``mutex()``. ``getLockedPtr()`` and the ``auto_enable``
  parameter of ``set()`` are removed. A ``LoggedValue`` can't be moved or
  default-constructed.
* ``RegistrationID`` is opaque: no ``first_index``, ``fields_count`` or ``operator+=``.
* ``CustomSerializerT(std::string)`` is ``explicit``, ``CustomTypeName<T>::get()`` is not
  ``constexpr``, ``Schema::custom_types`` and ``custom_schemas`` are ``std::map`` (were
  ``std::unordered_map``), and ``long double`` is a compile error. Removed:
  ``getPointerType()``, ``SerializeMe::enable_if_same_t`` and the library's
  ``AddFieldToHash()``, replaced by ``SchemaTextHash()`` and ``ComputeSchemaHash()`` (the
  parser keeps its own, for version 4 texts).
* Parser: ``ParseSnapshot()`` loses its unused ``callback_custom`` parameter and is
  ``[[nodiscard]]``; ``ParseSnapshotRecursive`` moves to ``DataTamerParser::detail`` and
  ``NullCustomCallback`` is removed; ``SnapshotView::schema_hash`` is ``uint64_t``;
  ``BuilSchemaFromText()`` is a deprecated alias of ``BuildSchemaFromText()``. The header
  needs a little-endian host and no longer includes ``<iostream>``.
* ``sinks/ros2_publisher_sink.hpp`` no longer includes the ``data_tamer_msgs`` headers.

**Behaviour changes**

* ``MCAPSink`` continues in a new numbered file at every reset (600 s by default) instead
  of truncating its file, so disk use is unbounded; ``setCreateNewFileOnReset(false)``
  truncates as before. Rollover files are named ``run_1.mcap`` (1.x: ``run.mcap_1``) and
  skip names that exist; the path passed to ``create()`` is still overwritten.
* ``SinkWorker::stop()`` delivers what is queued, then finishes the sink: ``MCAPSink``
  closes its file, and ``start()`` continues in the next numbered file. Destroying a
  running worker stops it.
* A channel's snapshot pool bounds the memory of its sinks: by default as many slots as
  fit in 8 MiB, from 64 to 1024, per channel. A sink that falls behind loses snapshots
  (``pool_exhausted``) instead of growing a queue.
* ``ROS2PublisherSink``: the data topic is reliable ``KeepLast(100)`` (was ``KeepAll``;
  ``ROS2PublisherOptions::data_qos``), and the ``schemas`` topic is transient-local
  ``KeepLast(1)``, published by ``startLogging()``.
* ``LoggedValue::set()`` never re-enables a disabled value. ``set()`` and ``get()`` of a
  struct or vector lock the channel's write mutex, so they wait for a snapshot in
  progress.
* An id kept across ``unregister()`` and a new registration of the same name is stale:
  ``setEnabled()`` and ``unregister()`` throw ``std::invalid_argument`` (1.x acted on the
  new registration).
* A snapshot taken by a thread that holds the write mutex (``scopedWrite()``, a
  ``LoggedValue`` guard) returns ``blocked``; in 1.x it deadlocked.
* From a sink callback, a control call that would wait for that callback's own worker
  throws ``std::logic_error`` and changes nothing instead of deadlocking (see ``DataSink``
  in ``data_sink.hpp``).
* Value names, custom type names and field names must be non-empty and free of whitespace
  and control characters (1.x refused only spaces); a channel name can't be empty or hold
  a control character. Two C++ types with one custom type name in a channel make
  ``registerValue()`` throw.
* A channel holds at most 8 sinks, and a registry at most 8 default sinks.
  ``addDefaultSink()`` also attaches the sink to the channels that exist already.
* MCAP messages carry a sequence number, 1, 2, 3, ... per channel and file (was always 1).
  The MCAP sinks don't write a snapshot with a negative timestamp; they count it in
  ``errors()``.
* Wire format: the schema text is version 5 and its hash is FNV-1a 64 of the text, the
  same on every platform. A ``std::string`` is written as ``char[]``, a ``uint32`` count
  and the characters. 1.x readers reject 2.0 files: use PlotJuggler 4 or a 2.0 parser.
* The parsers reject a schema text without a ``### version:`` line (releases before
  0.7.0). ``ParseSnapshot()`` returns false when bytes are left after the last field, and
  throws on malformed data.

**New**

* ``LogChannel::startLogging()`` freezes the schema, allocates the pool and announces the
  schema; the first ``takeSnapshot()`` or ``tryTakeSnapshot()`` with a sink attached
  calls it. ``tryTakeSnapshot()``, for real-time threads, never blocks or allocates once
  logging started, also when data_tamer is loaded with ``dlopen()``; the call that starts
  logging is not real-time (RealtimeSanitizer reports it), so call ``startLogging()``
  before the loop. Without a sink it returns ``no_sinks``, in real time; if starting
  throws, ``start_failed``, and the next call retries. Several threads may take snapshots
  of one channel.
* ``accepted(SnapshotResult)``, ``constexpr`` and real-time safe: true for ``ok`` and
  ``partial``, what ``stats().accepted`` counts.
* ``tryTakeSnapshot()`` is ``noexcept``: a serializer that throws ends it with
  ``SnapshotResult::serialize_failed``, counted by ``serializeFailures()``;
  ``takeSnapshot()`` lets the exception through, as in 1.x. The library catches it, so
  serializers compiled into binaries built against earlier headers are covered too.
* ``LogChannel::scopedWrite()`` groups writes from another thread into one snapshot.
  ``registerValue()`` accepts a ``const std::atomic<T>*``.
* ``setEnabled()`` is lock-free; new ``trySetEnabled()`` (``noexcept``) and
  ``isEnabled(id)``.
* ``setPoolCapacity()`` (a count, or a stall tolerance and a period),
  ``setPayloadCapacity()`` and ``ChannelsRegistry::setChannelDefaults()``.
* ``LogChannel::stats()`` and ``SinkWorker::stats()``: snapshot attempts and accepted
  snapshots, pool, oversize, serialization-failure and lock counters, refusals per sink,
  deliveries, errors and queue high water.
* ``MCAPRingSink``, a flight recorder: it keeps the last ``window`` of every channel in
  RAM and writes an MCAP file on ``requestDump()``, which is real-time safe.
* ``ROS2PublisherSink`` takes ``ROS2PublisherOptions``: QoS, aggregation into
  ``data_tamer_msgs/SnapshotBatch`` (new message) and the schema format.
* A YAML rendering of the schema (wire format section 2.1, also version 5): ``ToYaml()``,
  ``RenderSchema()``, ``SchemaFormat::Yaml``.
* ``DataSink::onStop()`` and ``onStart()``, ``SinkWorker::Delivery::Manual`` (no thread:
  the application calls ``drain()``).
* ``ChannelsRegistry::stopAll()`` and ``shutdown()`` (``stopAll()`` then ``clear()``, for
  the end of ``main()``: every sink is finished and no channel is left borrowing your
  variables).
* ``TypeDefinitionTrait<T>`` describes a type without reopening its namespace (Eigen,
  ...).
* Real-time checks with Clang 20 or later: the real-time API is declared
  ``[[clang::nonblocking]]`` (``DATA_TAMER_NONBLOCKING``), so in a function marked
  ``[[clang::nonblocking]]`` ``-Wfunction-effects`` rejects a call to ``takeSnapshot()``,
  a registration or a non-scalar ``LoggedValue::set()``, and RealtimeSanitizer
  (``-fsanitize=realtime``) reports every allocation, lock or system call at run time,
  in data_tamer and in custom serializers. On Linux (ELF) data_tamer declares to RTSan
  the futex calls the real-time contract allows. A new ``rtsan`` preset and CI job.
* New headers: ``names.hpp`` (``JoinNames()`` and ``IsCanonicalName()`` build and check
  hierarchical names), ``sinks/mcap_encoding.hpp`` (the MCAP encoding of ``MCAPSink``, for
  code that writes data_tamer MCAP files itself) and ``fwd.hpp`` (forward declarations).
* Decoding in C++ in a few lines: ``data_tamer_v2/mcap_reader.hpp`` reads an MCAP file
  (``ForEachValue()`` decodes each value, ``ForEachSnapshot()`` hands over each snapshot),
  in log time order (a crashed recording up to its last complete chunk, in file order),
  optionally restricted to some topics, with every error a
  ``std::runtime_error`` naming the file and the topic, like Python's ``iter_mcap()``;
  ``data_tamer_v2/decode.hpp`` has ``ToSnapshotView()``, the parser's view of a
  ``Snapshot`` in a custom sink. T07 and T08 use them.
* Parser: ``SchemaRegistry``, ``DataTamerParser::ToSnapshotView()`` of a ROS 2 message and
  ``ForEachSnapshotInBatch()`` decode the ROS 2 messages without ROS;
  ``SplitMcapMessage()`` splits an MCAP message.
* Parser: ``ParseSnapshot()`` decodes several times faster and allocates nothing per
  value (``benchmarks/parser_benchmark.cpp`` measures it); its depth limit no longer
  depends on the order of the fields.
* Text: ``std::string``, any other ``std::basic_string<char>`` (``std::pmr::string``) and
  ``std::string_view`` are recorded as the new basic schema type ``string`` (a ``uint32``
  byte count and the bytes): registered, as members of custom types, in a
  ``std::vector`` or ``std::array``, and as ``LoggedValue<std::string>``. Their
  registration now also compiles with Clang 18 and older. ``ParseSnapshot()`` and
  ``ForEachValue()`` take an optional string callback that gets a ``std::string_view``
  into the payload (without one, text is skipped); the Python decoder returns a ``str``
  and the TypeScript decoder and the Foxglove extension a string, decoded as UTF-8 with
  invalid bytes replaced by U+FFFD. ``std::vector<char>`` and ``std::array<char, N>``
  stay one value per character. ``BasicType::STRING`` is appended to the enum. A custom
  type named after a basic type is refused. ``docs/wire_format/vectors/`` has a strings
  vector.
* Python decoder, standard library only: ``pip install "data-tamer-parser[mcap]"``.
  ``parse_snapshot()`` compiles each schema and active mask into ``struct`` formats,
  5 to 15 times faster than a value-by-value walk; ``parse_snapshot_values()`` returns
  names and values as tuples, for column-oriented code.
* Foxglove extension in ``data_tamer_foxglove/extension/`` for ROS 2 topics and
  rosbag2 recordings: decoded snapshots with header timestamps, schema catalogs,
  batch inspection, and packaged ``.foxe`` release artifacts.
* TypeScript snapshot decoder in ``data_tamer_foxglove/``, with schema versions
  4 and 5 (line format and YAML), bounded compiled layouts, nested objects and ROS 2 message helpers.
* Tutorials ``T01`` to ``T08``, run by ctest; ``docs/USER_MANUAL.md``,
  ``docs/llm_user_manual.md``, ``docs/MIGRATION.md`` and the wire format specification
  ``docs/wire_format.md``, with golden vectors.

**Fixed**

* ``long long``, ``unsigned long long``, ``wchar_t``, ``char16_t``, ``char32_t`` and enums
  over them were recorded as ``other``, which no reader decodes.
* A ``std::string`` was written with a 16-bit length that no reader decoded.
* Two C++ types with one custom type name in a channel: the second was serialized with the
  first one's serializer.
* A registration that threw could leave a custom type in the schema; it now leaves the
  channel unchanged.
* The schema text and its hash depended on the global C++ locale.
* Concurrent ``takeSnapshot()`` calls on one channel raced on its snapshot buffer.
* The parser read past the buffer before checking its size. Both decoders now check bounds
  before every read and reject masks shorter than the schema, oversized counts and cyclic
  custom types.

**Packaging and ABI**

* ROS 2: the package is ``data_tamer_v2`` and installs beside ``data_tamer_cpp`` 1.x (the
  library is ``libdata_tamer_v2.so.2``). Link ``data_tamer_v2::data_tamer``, or use
  ``find_package(data_tamer 2)`` and ``data_tamer::data_tamer``. ``mcap_vendor`` is linked
  publicly; ``-DBUILD_TESTING=OFF`` builds no test. ``data_tamer_msgs`` keeps its name.
* Without ROS: package ``data_tamer``, target ``data_tamer::data_tamer``, SONAME
  ``libdata_tamer.so.2`` (1.x had none). Building needs CMake 3.22 and C++20; consumers
  need C++17. ``-Werror`` is opt-in (``DATA_TAMER_WERROR``). As a subproject
  (``add_subdirectory()``, FetchContent) data_tamer installs nothing and builds no test,
  example or ROS 2 sink.
* Outside ROS 2, MCAP is 2.1.3 (1.x bundled 1.3.0). CMake downloads it, pinned by hash
  (``FETCHCONTENT_SOURCE_DIR_DATA_TAMER_MCAP`` for offline builds), unless
  ``DATA_TAMER_USE_SYSTEM_MCAP`` is on (conan). The library contains the MCAP
  implementation and installs its headers: don't define ``MCAP_IMPLEMENTATION``. The
  ``mcap_lib`` target is gone; zstd and lz4 are linked privately. The vendored moodycamel
  queue is removed.
* ABI: within 2.x a newer library runs binaries built against older 2.x headers.
  ``tests/abi_tests.cpp`` pins the layouts and the ``abi`` CI job (libabigail) compares
  each change with the last 2.x release. The ``DataSink`` and ``CustomSerializer`` vtables
  and the MCAP version are frozen; the ``stats()`` functions are inline and built from
  exported getters, so their structs can grow. ``ROS2PublisherSink`` also follows the
  rclcpp ABI. Header defaults apply to rebuilt consumers only, Pimpl defaults to old
  binaries too.
* ``SinkWorkerOf<T>`` has the layout of ``SinkWorker`` (pinned in
  ``tests/abi_tests.cpp``), and the factories that return it are inline.
* One version number for the library, the ROS packages, conan, the Python and TypeScript
  decoders and the Foxglove extension, checked by ``tools/check_versions.py``. Pushing a
  tag ``X.Y.Z`` creates the GitHub release, whose notes are this section
  (``tools/release_notes.py``).

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
