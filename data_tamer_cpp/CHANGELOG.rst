^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
Changelog for package data_tamer
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Unreleased
----------
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
