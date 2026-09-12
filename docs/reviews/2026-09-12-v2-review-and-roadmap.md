# V2 review and roadmap (external review, 2026-09-12)

Produced by an independent reviewer (Codex, gpt-5.6) from a read-only checkout of `V2` at `fc29cba`, on request: settle every breaking change before 2.0.0 and propose a long-term roadmap. Kept verbatim as a working document; items are tracked as issues as they are accepted or rejected.

---

# Part 1 — Review for 2.x stability

Review target: `fc29cbab78649cb34c0ff15d5851e37ffd028dd1`, compared with `origin/main` at `a91a49ec7e19b8f64383041a4b4a678b20324516`. The comparison contains 65 changed files, with 7,516 insertions and 858 deletions.

**I would not release this revision as 2.0.0.** The blockers include unsafe parsing, incorrect size calculations, incomplete registration rollback, inconsistent transaction semantics, and sink behavior that can silently lose data. Separately, several public implementation details would make the proposed future changes unnecessarily disruptive.

“MUST 2.0” below means settle or fix this before releasing 2.0.0. Some are compatible bug fixes; others require using the present API/ABI break. “Can wait” means the implementation can arrive in 2.x if the specified boundary is established now. It is neither possible nor useful to implement every conceivable future feature before 2.0.

File references identify the reviewed revision. Sink files are sometimes cited by basename.

## A. Correctness and robustness blockers

### 1. The C++ parser checks payload bounds after reading

**Locations:** `data_tamer_cpp/include/data_tamer_parser/data_tamer_parser.hpp:126`, `data_tamer_cpp/include/data_tamer_parser/data_tamer_parser.hpp:181`, `data_tamer_cpp/include/data_tamer_parser/data_tamer_parser.hpp:466`; `data_tamer_cpp/include/data_tamer_parser/data_tamer_parser.hpp:53`.

`Deserialize<T>()` performs `memcpy` before checking that the buffer contains `sizeof(T)` bytes. A truncated payload therefore causes an out-of-bounds read before the intended exception. `GetBit()` reads the mask without checking its length. `BufferSpan::trimFront()` advances the pointer and subtracts from an unsigned size without validating the request.

These are trust-boundary defects: the parser processes recorded files and received messages, including corrupt or truncated ones.

**Concrete change:** Check lengths before every read or span advance. Validate that the active mask covers all schema fields before entering the field loop. Centralize this in checked span operations rather than fixing individual callers. Define whether additional mask bytes and unused high bits are accepted.

**Timing:** **MUST 2.0**, compatible correctness fix. Add truncated-header, truncated-mask, truncated-scalar and truncated-vector cases under ASan/UBSan.

### 2. Parsing needs resource limits and a consistent failure contract

**Locations:** `data_tamer_cpp/include/data_tamer_parser/data_tamer_parser.hpp:405`, `data_tamer_cpp/include/data_tamer_parser/data_tamer_parser.hpp:453`; `python/data_tamer_parser.py:129`, `python/data_tamer_parser.py:148`.

The C++ parser recursively follows custom-type definitions without rejecting cycles or limiting depth. A dynamic array of an empty custom type can demand enormous iteration while consuming essentially no payload. The Python decoder constructs a list of indexed names from the encoded count before decoding their values; a four-byte count can request billions of strings.

Failure behavior is inconsistent: C++ returns `false` for a hash mismatch or trailing bytes, throws for other failures, and may already have invoked callbacks when failure is discovered. Python exposes a mixture of format errors and incidental exceptions such as mask indexing errors.

**Concrete change:** Validate the schema graph once; reject cycles; impose configurable limits on depth, fields, elements, decoded bytes and name lengths. Iterate Python array indices lazily. Return a documented parse result with an error category and byte/field location. Explicitly choose whether callbacks may have run on failure; offer validation before delivery if transactional consumption is required.

**Timing:** **MUST 2.0** for safe defaults and the error contract. More sophisticated diagnostics can wait.

### 3. Schema parsing accepts ambiguous or malformed declarations

**Locations:** `data_tamer_cpp/include/data_tamer_parser/data_tamer_parser.hpp:276`, `data_tamer_cpp/include/data_tamer_parser/data_tamer_parser.hpp:313`, `data_tamer_cpp/include/data_tamer_parser/data_tamer_parser.hpp:349`, `data_tamer_cpp/include/data_tamer_parser/data_tamer_parser.hpp:380`; `python/data_tamer_parser.py:54`.

Primitive types are recognized by prefix. A custom type named `float64Pose`, for example, can be interpreted as `float64`. The compatibility path also examines the other token for uppercase primitive prefixes. Section delimiters are recognized by substring, numeric parsing accepts more than the intended grammar, and fixed-array counts are narrowed to `uint16_t`.

The Python parser accepts a different set of malformed declarations. That makes “the format” dependent on the decoder.

**Concrete change:** Parse exact tokens with a single documented grammar. Validate the entire numeric token and its range. Reject duplicate fields, duplicate incompatible type definitions, malformed brackets, missing required headers and unknown types. Keep permissive historical parsing behind an explicit legacy mode.

**Timing:** **MUST 2.0**. Restricting previously accepted malformed input is preferable to making its interpretation permanent.

### 4. The 64-bit schema hash is stored in platform-sized fields

**Locations:** `data_tamer_cpp/include/data_tamer/data_sink.hpp:33`; `data_tamer_cpp/include/data_tamer_parser/data_tamer_parser.hpp:92`, `data_tamer_cpp/include/data_tamer_parser/data_tamer_parser.hpp:323`; `data_tamer_cpp/include/data_tamer/types.hpp:137`.

`Schema::hash` is `uint64_t`, but `Snapshot::schema_hash` and `SnapshotView::schema_hash` are `size_t`. A 32-bit build cannot carry a general version-5 hash.

The parser uses `std::stoul` for the declared hash. On Windows’ LLP64 model, `unsigned long` is 32 bits even in a 64-bit process.

**Concrete change:** Use `uint64_t` end to end, preferably through a `SchemaHash` type. Parse directly into a checked 64-bit destination, consuming the entire token.

**Timing:** **MUST 2.0**, including the public layout change.

### 5. Canonical hashing is not identical across implementations

**Locations:** `data_tamer_cpp/src/types.cpp:96`; `data_tamer_cpp/include/data_tamer_parser/data_tamer_parser.hpp:215`; `python/data_tamer_parser.py:66`; `docs/wire_format.md:214`.

C++ removes the first occurrence of `### hash:` and the remainder of that line, without requiring that occurrence to be the actual header. Python removes every line beginning with `### hash:`. An opaque custom schema containing such a line is therefore hashed differently.

Schema emission also uses stream formatting without explicitly fixing the locale. Numeric array sizes should not depend on the process’s global locale.

Finally, a 64-bit hash is not collision-free, despite the wording in the specification.

**Concrete change:** Define the header structurally and remove exactly that header’s bytes. Preserve opaque schema bytes verbatim. Make canonical output locale-independent. Publish cross-language fixtures containing header-like text inside opaque schemas. Treat the hash as an identifier with collision detection when schemas enter a catalog: equal hash plus unequal canonical text must be an error.

**Timing:** **MUST 2.0**. Finalize this before version 5 becomes a released artifact.

### 6. Fixed-size discovery uses an uninitialized accumulator

**Location:** `data_tamer_cpp/include/data_tamer/custom_types.hpp:123`.

The fixed-array branch declares `size_t obj_size;` and passes it to recursive code that adds to it. This reads an indeterminate value. The result feeds cached serialized sizes and therefore allocation and strict-capacity decisions.

Existing mixed custom-type tests do not establish correctness for entirely fixed-size nested arrays.

**Concrete change:** Initialize every accumulator and use checked multiplication and addition. Test a fixed custom structure containing fixed arrays, including nested arrays, against the actual emitted byte count.

**Timing:** **MUST 2.0**, compatible correctness fix.

### 7. Several serialized-size calculations do not describe the data actually written

**Locations:** `data_tamer_cpp/include/data_tamer/contrib/SerializeMe.hpp:354`, `data_tamer_cpp/include/data_tamer/contrib/SerializeMe.hpp:361`; `data_tamer_cpp/include/data_tamer/values.hpp:143`; `data_tamer_cpp/src/channel.cpp:453`.

`BufferSize(std::array<T, N>)` computes the size from `T{}` rather than the actual elements. That is wrong when the elements have variable-length members.

The vector shortcut uses `sizeof(T)` for trivially copyable elements, but a reflected custom type can be trivially copyable while its serialized representation excludes padding or includes only selected members. The size pass and serialization pass then disagree.

The channel’s final resize can hide overestimation, but it does not make the calculation correct. Overestimation can cause unnecessary strict-mode drops; underestimation can cause serialization failure.

**Concrete change:** Use the same type rules for sizing and writing. Only use `sizeof(T)` when the wire representation is explicitly the object’s fixed-width primitive representation. Size variable elements from the actual instances. Check arithmetic overflow before allocation.

**Timing:** **MUST 2.0**.

### 8. Array limits are silently narrowed, and empty fixed arrays are ambiguous

**Locations:** `data_tamer_cpp/include/data_tamer/values.hpp:87`, `data_tamer_cpp/include/data_tamer/values.hpp:97`, `data_tamer_cpp/include/data_tamer/values.hpp:266`, `data_tamer_cpp/include/data_tamer/values.hpp:278`; `docs/wire_format.md:41`.

Array lengths are stored in `uint16_t`. A larger `std::array` silently changes shape when narrowed; 65,536 becomes zero, which elsewhere means a dynamically sized vector. Zero-length fixed arrays collide with the same sentinel and can reach code using `front()`.

The specification limits fixed arrays to 1–65,535, but the public templates do not consistently enforce that contract.

**Concrete change:** Immediately reject unsupported extents at compile time. Prefer an explicit shape representation—scalar, dynamic sequence, fixed sequence with an extent—rather than `is_vector` plus a zero sentinel. Decide whether the intended format limit really is 65,535.

**Timing:** **MUST 2.0** for validation and public shape representation. Supporting larger extents can wait if a future wire version is explicit.

### 9. Accepted C++ types and supported wire types disagree

**Locations:** `data_tamer_cpp/include/data_tamer/types.hpp:65`, `data_tamer_cpp/include/data_tamer/types.hpp:94`; `data_tamer_cpp/include/data_tamer/channel.hpp:293`; `data_tamer_cpp/include/data_tamer/custom_types.hpp:78`; `data_tamer_cpp/include/data_tamer/contrib/SerializeMe.hpp:572`.

`IsNumericType` accepts arithmetic types more broadly than `GetBasicType` maps them. Types such as `long double` can enter a numerical registration path without a valid primitive schema representation.

Other template paths advertise more container generality than they implement. Nested numeric containers encounter custom-type discovery that expects a `TypeDefinition`; `vector<bool>` reaches operations incompatible with its specialized storage.

**Concrete change:** Define one supported-type trait that drives registration, schema construction, sizing and serialization. Reject unsupported types at the registration boundary with useful diagnostics. Publish a support matrix covering scalar, atomic, enum, vector, fixed array, nested container and reflected custom-type combinations.

**Timing:** **MUST 2.0** for consistent rejection. Additional supported combinations can wait.

### 10. Endianness and bounds behavior depend on the serialization path

**Locations:** `data_tamer_cpp/include/data_tamer/values.hpp:103`; `data_tamer_cpp/include/data_tamer/contrib/SerializeMe.hpp:179`, `data_tamer_cpp/include/data_tamer/contrib/SerializeMe.hpp:450`; `data_tamer_cpp/include/data_tamer_parser/data_tamer_parser.hpp:126`; `docs/wire_format.md:47`.

Direct scalar and atomic serialization copies native object bytes. The generic serialization layer has separate endian-handling machinery. The wire contract says little-endian, so these paths cannot be treated as interchangeable on every architecture.

Some public serialization paths also copy bytes before validating the destination or source span, including byte-vector deserialization.

**Concrete change:** Route primitive reads and writes through shared checked little-endian operations. Encode booleans canonically and validate their representation when decoding. Check dynamic counts before narrowing to `uint32_t`. Avoid arithmetic overflow in count-by-element-size calculations.

**Timing:** **MUST 2.0** for contract correctness. Native big-endian support may be deferred only with an explicit, enforced platform restriction.

### 11. Registration does not provide exception safety

**Location:** `data_tamer_cpp/src/channel.cpp:124`.

Registration mutates the series array, flags, name map and schema fields before calling user-provided `typeSchema()` and before completing the hash update. If a later operation throws, the channel can retain a registered pointer even though registration never returned an ID.

For an unsuccessful `LoggedValue` construction, that pointer can refer to storage whose construction has unwound. Retrying the same name can also fail because the partial registration remains present.

**Concrete change:** Perform throwing discovery and schema construction before publication. Commit the pointer, flags, lookup entry and schema together, or roll them all back. A failed registration must leave the channel observably unchanged.

**Timing:** **MUST 2.0**.

### 12. Serializer identity is not strong enough to protect type safety or the wire schema

**Locations:** `data_tamer_cpp/include/data_tamer/custom_types.hpp:193`, `data_tamer_cpp/include/data_tamer/custom_types.hpp:214`; `data_tamer_cpp/include/data_tamer/values.hpp:281`; `data_tamer_cpp/src/channel.cpp:161`.

The type registry caches serializers by textual type name. Distinct C++ types using the same name can receive the wrong `CustomSerializerT<T>`, whose implementation casts a `void*` to its own expected type.

Re-registration compares the stored value’s C++ type and shape, but does not establish that the replacement custom serializer has the same wire identity. A slot can therefore retain its frozen schema while changing its encoding.

**Concrete change:** Validate both C++ type identity and canonical wire identity during registration. Reject conflicting names. When rebinding a frozen slot, require the same serializer contract and schema. Reject null serializers rather than falling back to raw object representation.

**Timing:** **MUST 2.0**.

### 13. Transactions do not cover enable-state changes coherently

**Locations:** `data_tamer_cpp/src/channel.cpp:435`, `data_tamer_cpp/src/channel.cpp:441`; `data_tamer_cpp/include/data_tamer/channel.hpp:217`, `data_tamer_cpp/include/data_tamer/channel.hpp:465`; `data_tamer_cpp/tests/transaction_tests.cpp:270`.

The snapshot refreshes its active mask before taking the write mutex. `LoggedValue::set()` can enable a value. Consequently, a transaction that updates and enables multiple previously disabled values can overlap mask reconstruction.

A snapshot can build a mask containing only the first enabled field, then wait for the transaction, and finally serialize the post-transaction values under that mixed mask. The documented “all or none” grouping guarantee is broader than the implementation. Tests of already-enabled paired values do not cover this case.

**Concrete change:** Make the mask and serialized values participate in the same transaction boundary. Define separately what independent `setEnabled()` calls guarantee. Add a controlled interleaving test with initially disabled values enabled inside one transaction.

**Timing:** **MUST 2.0**.

### 14. MCAP write failures and sink rejection are not reported consistently

**Locations:** `data_tamer_cpp/src/data_sink.cpp:32`; `data_tamer_cpp/include/data_tamer/data_sink.hpp:88`, `data_tamer_cpp/include/data_tamer/data_sink.hpp:98`; `mcap_sink.cpp:145`.

`storeSnapshot()` returns `bool`, but the dispatcher ignores it. `storeErrors()` counts exceptions only. MCAP’s write result is also ignored, and the callback returns success.

There is therefore no coherent distinction between accepted into the queue, processed by the callback and successfully written. The channel’s `true` result only establishes enqueue acceptance.

**Concrete change:** Define delivery stages explicitly. Either make the callback return a meaningful `StoreResult` that is counted and exposed, or make it `void` with a documented failure mechanism. Check every MCAP write/open/finalization status. Expose the latest error and accumulated failures through a non-RT diagnostic API.

**Timing:** **MUST 2.0** for the callback and error semantics.

### 15. The default file lifecycle can discard recordings

**Locations:** `mcap_sink.hpp:37`, `mcap_sink.hpp:40`, `mcap_sink.hpp:48`, `mcap_sink.hpp:53`; `mcap_sink.cpp:52`, `mcap_sink.cpp:87`, `mcap_sink.cpp:184`.

The default periodic reset overwrites the file after 600 seconds. That is a surprising and destructive default for a recording library.

The MCAP destructor stops the worker but does not establish an explicit close-admission-and-drain sequence. Depending on where the worker is when it stops, queued samples are not guaranteed to be processed. `stopRecording()` and `finishQueueAndStop()` also have materially different acceptance and draining semantics.

**Concrete change:** Default to preserving recorded data. Make truncation and bounded retention explicit options. Provide one documented lifecycle with separate operations for pausing acceptance, flushing accepted work, closing the file and starting another recording. Define destructor behavior; provide an explicit close operation that can report finalization failures.

**Timing:** **MUST 2.0**, because defaults and lifecycle semantics are difficult to reverse later.

### 16. MCAP configuration and restart need synchronization and rollback

**Locations:** `mcap_sink.cpp:70`, `mcap_sink.cpp:149`, `mcap_sink.cpp:163`, `mcap_sink.cpp:168`, `mcap_sink.cpp:201`.

The reset configuration setters write state read by the worker without using the worker’s protecting mutex. Serializing control calls with each other does not synchronize them with an active callback.

Restart replaces the writer before the new file has successfully opened and been initialized. Failure can destroy a functioning recording session and leave the object in an unclear state. Rotation timing uses a wall clock, which can jump.

**Concrete change:** Synchronize mutable options or restrict them to a stopped state and enforce that restriction. Build and initialize a candidate writer before committing it. Use a steady clock for elapsed rotation intervals and recording timestamps only for message time.

**Timing:** **MUST 2.0** for race and failure-state fixes. Richer rotation policies can wait.

### 17. Sink schema catalogs incorrectly equate channel name with schema identity

**Locations:** `mcap_sink.cpp:45`, `mcap_sink.cpp:93`, `mcap_sink.cpp:201`; `data_tamer_cpp/src/data_tamer.cpp:33`, `data_tamer_cpp/src/data_tamer.cpp:50`; `data_tamer_msgs/msg/Schema.msg:1`.

A sink can receive distinct live channels with the same name: `LogChannel::create()` permits that directly, and clearing the registry does not destroy channels held elsewhere.

The MCAP catalog retains schemas by name. A newer schema replaces the catalog entry for an older live channel; reopening the file can then rebuild only the newer schema. Subsequent snapshots from the older channel no longer have the required mapping. The ROS catalog has the same name-based identity issue.

**Concrete change:** Store schemas by their validated schema identity. Treat the channel name as a label, not a unique lifetime identifier. Decide whether duplicate named streams are rejected at attachment or distinguished by an explicit stream ID.

**Timing:** **MUST 2.0**.

### 18. ROS schema publication is delayed and its retention policy is unbounded at the library level

**Locations:** `data_tamer_msgs/msg/Schema.msg:1`, `data_tamer_msgs/msg/Schemas.msg:1`, `data_tamer_msgs/msg/Snapshot.msg:1`; `data_tamer_cpp/tests/ros2_publisher_tests.cpp:1`.

The publisher’s `addChannel()` records a changed catalog, but schema publication occurs on a subsequent snapshot. Registering a schema does not itself publish it. The changed flag is cleared before publication succeeds, so a thrown publication operation can suppress retries until another schema change.

The schema publisher uses reliable transient-local `KeepAll`, while each publication contains the accumulated schema catalog. Retaining successive complete catalogs can grow quadratically with catalog growth, subject to middleware limits. The data publisher also uses `KeepAll` without a library-level bound.

**Concrete change:** Publish schemas independently of sample delivery, retry until successful, and establish the intended schema-before-data behavior. Use a bounded default history—typically the latest complete catalog for schemas—and expose a bounded data QoS configuration. Test actual subscribers, late joining and publication failure; constructor tests do not establish these guarantees.

**Timing:** **MUST 2.0** for default resource bounds and publication guarantees. Additional QoS presets can wait.

### 19. The shipped serialization helper contains additional broken public paths

**Locations:** `data_tamer_cpp/include/data_tamer/contrib/SerializeMe.hpp:383`, `data_tamer_cpp/include/data_tamer/contrib/SerializeMe.hpp:403`, `data_tamer_cpp/include/data_tamer/contrib/SerializeMe.hpp:450`; `data_tamer_cpp/include/data_tamer_parser/data_tamer_parser.hpp:72`.

Reflected-object deserialization constructs its field callback with a const-qualified pointee and then passes that field to a mutating deserializer. That path is not a dependable inverse of reflected serialization.

The parser declares `TypeField::operator!=` without supplying its definition. Clients using that advertised operation get a link failure.

**Concrete change:** Fix and instantiate the intended public operations in compile tests. Alternatively, stop presenting unused helper functionality as part of the supported public surface. Do not let uninstantiated templates substitute for verified support.

**Timing:** **MUST 2.0** for the shipped public API; compatible fixes.

## B. Public API decisions to settle now

### 20. `RegistrationID` is a writable range, not a safe registration identity

**Locations:** `data_tamer_cpp/include/data_tamer/types.hpp:101`, `data_tamer_cpp/include/data_tamer/types.hpp:164`; `data_tamer_cpp/src/channel.cpp:205`; `data_tamer_cpp/include/data_tamer/details/shared_state.hpp:115`.

The public fields allow fabricated, out-of-range and cross-channel IDs. `operator+=` combines counts without establishing contiguity or common ownership. A stale ID can affect a replacement registration occupying the same slot. Runtime operations trust these fields when indexing internal storage.

The provided hash specialization does not establish a complete ordinary value-type contract; equality is also needed for default unordered-container use.

**Concrete change:** Use an opaque registration handle with channel identity and a generation, or explicitly expose a stable `FieldID` and make rebinding a separate operation. Provide a separate checked group type if grouped enable/disable is required. Define stale-handle behavior and make invalid IDs fail safely.

**Timing:** **MUST 2.0**. Changing whether an ID denotes a field slot or a particular registration later would be a semantic break.

### 21. Borrowed registrations need an explicit lifetime contract

**Locations:** `data_tamer_cpp/include/data_tamer/channel.hpp:80`, `data_tamer_cpp/include/data_tamer/channel.hpp:132`, `data_tamer_cpp/include/data_tamer/channel.hpp:145`, `data_tamer_cpp/include/data_tamer/channel.hpp:444`.

Raw-pointer registration permits nulls and requires users to keep storage alive until unregister has completed. `createLoggedValue()` uses shared ownership, while its destructor performs a blocking unregister when the channel remains alive.

That hidden destructor work matters if the last `shared_ptr` is released on an RT thread or inside a write transaction.

**Concrete change:** Distinguish borrowed registration from owned values in names and documentation. Prefer a reference-taking convenience overload for non-null borrowed objects. Provide a scoped registration handle with explicit reset/unregister, and document which destructor paths can block. Keep shared ownership where lifetime extension is intentional; replacing every `shared_ptr` with a reference would create different lifetime bugs.

**Timing:** **MUST 2.0** for ownership and destruction guarantees. Additional convenience overloads can wait.

### 22. `LoggedValue::set()` has a surprising second job

**Locations:** `data_tamer_cpp/include/data_tamer/channel.hpp:465`; `data_tamer_cpp/include/data_tamer/logged_value.hpp:1`.

A normal write auto-enables the field by default. A controller can disable a field only for the next producer write to re-enable it. The boolean option also conceals two distinct operations in an ordinary setter.

**Concrete change:** Make `set()` update the value only. If automatic enabling is useful, provide an explicitly named operation or a clearly named registration policy. Define “disabled” independently from “has never received a value.”

**Timing:** **MUST 2.0** if changing the default. Preserving the current default into 2.x makes this behavior part of the contract.

### 23. The public locking APIs are not composable

**Locations:** `data_tamer_cpp/include/data_tamer/channel.hpp:215`, `data_tamer_cpp/include/data_tamer/channel.hpp:217`, `data_tamer_cpp/include/data_tamer/channel.hpp:498`; `data_tamer_cpp/include/data_tamer/details/locked_reference.hpp:23`, `data_tamer_cpp/include/data_tamer/details/locked_reference.hpp:59`; `data_tamer_cpp/include/data_tamer/details/shared_state.hpp:32`.

Transactions recognize same-channel nesting through a thread-local chain. Non-scalar pointer guards instead lock the underlying non-recursive mutex directly. Natural combinations—such as acquiring a mutable pointer inside `scopedWrite()`—can deadlock.

The guards and transactions also borrow their underlying objects. Holding a guard does not necessarily keep the value or channel state alive.

**Concrete change:** Route all channel-aware write guards through one nesting mechanism. Expose a channel transaction rather than the concrete mutex. State and enforce guard lifetime requirements, or retain the required owner. A callback-based `modify()` operation may eliminate several escaping-pointer cases.

**Timing:** **MUST 2.0** for the public guard model. Choosing a recursive PI mutex instead of the thread-local chain can wait if that implementation is hidden.

### 24. The global `Mutex` alias and internal-state escape hatches should disappear

**Locations:** `data_tamer_cpp/include/data_tamer/details/locked_reference.hpp:12`; `data_tamer_cpp/include/data_tamer/channel.hpp:215`, `data_tamer_cpp/include/data_tamer/channel.hpp:263`; `data_tamer_cpp/include/data_tamer/details/shared_state.hpp:75`.

An installed header introduces `Mutex` in the global namespace. `writeMutex()` exposes the concrete synchronization primitive. `sharedState()` exposes a shared owner of an implementation structure with publicly accessible mutex and registration flags.

Clients can therefore depend on, or directly mutate, the mechanism the proposed hot-path changes need to replace.

**Concrete change:** Remove the global alias. Remove public mutable access to `ChannelSharedState`. Expose only the required transaction and enable/query operations. Keep raw synchronization access internal; if an advanced escape hatch is indispensable, make its unsupported nature explicit and keep it outside the stable API.

**Timing:** **MUST 2.0**.

### 25. Snapshot ownership should be represented in the type system

**Locations:** `data_tamer_cpp/include/data_tamer/data_sink.hpp:28`; `data_tamer_cpp/include/data_tamer/data_sink.hpp:112`; `data_tamer_cpp/include/data_tamer/details/snapshot_pool.hpp:112`.

`Snapshot` owns its vectors but borrows its channel name through `string_view`. Copying it looks like making an owned record but does not own everything. A copied snapshot can outlive the name’s storage.

Safe retention instead requires calling `retainSnapshot()` during the callback, obtaining ownership through thread-local context and including an internal pool header.

**Concrete change:** Separate a borrowed `SnapshotView`, a retained immutable snapshot handle, and an explicitly owning copy if needed. Pass the retainable handle to the callback directly, or expose retention through the view itself. Do not make the retention contract depend on hidden TLS.

**Timing:** **MUST 2.0**, particularly the sink callback signature and public ownership types.

### 26. The worker-owning sink base is unsafe to extend casually

**Locations:** `data_tamer_cpp/include/data_tamer/data_sink.hpp:62`, `data_tamer_cpp/include/data_tamer/data_sink.hpp:100`; `data_tamer_cpp/src/data_sink.cpp:60`, `data_tamer_cpp/src/data_sink.cpp:84`; `data_tamer_cpp/tests/sink_queue_tests.cpp:1`.

The base class owns a thread that calls derived virtual functions. Every derived destructor must stop the thread before its own callback state is destroyed. A check in the base destructor detects a violation too late to provide a safe lifetime model.

The lifecycle is additionally divided across stop-worker, stop-admission, drain and reopen-admission operations. Calling them in the wrong order changes whether data is lost or shutdown waits indefinitely.

**Concrete change:** Prefer composition: a dispatcher owns a callback implementation and shuts down before destroying it. If retaining inheritance, establish a construction/destruction protocol that guarantees this ordering rather than relying on every derived destructor remembering it. Publish the callback threading and reentrancy contract.

**Timing:** **MUST 2.0**. Changing the extension base or adding virtual lifecycle methods later breaks sink implementations.

### 27. Callback visibility and controller reentrancy need tightening

**Locations:** `data_tamer_cpp/include/data_tamer/data_sink.hpp:85`, `data_tamer_cpp/include/data_tamer/data_sink.hpp:98`; `data_tamer_cpp/src/channel.cpp:214`, `data_tamer_cpp/src/channel.cpp:382`; `mcap_sink.hpp:35`.

Concrete sinks expose operations such as `storeSnapshot()` more broadly than the protected base contract. Direct calls bypass queued ownership and serialization guarantees.

Schema callbacks run while channel control locks are held. A custom sink that calls back into channel control can deadlock. Conversely, one sink can receive schema-registration calls from several channels; the required synchronization is not obvious from the callback signature.

**Concrete change:** Keep delivery callbacks non-public. Specify exactly which callbacks are serialized, which can overlap, and which controller operations are forbidden within them. Where possible, invoke user code outside internal control locks after creating a complete immutable schema object.

**Timing:** **MUST 2.0** for extension contracts. Internal lock restructuring can follow without breaking the API.

### 28. Preparation, snapshot acceptance and stopping are separate concepts

**Locations:** `data_tamer_cpp/src/channel.cpp:382`; `data_tamer_cpp/include/data_tamer/channel.hpp:183`, `data_tamer_cpp/include/data_tamer/channel.hpp:230`; `data_tamer_cpp/tests/channel_control_tests.cpp:313`.

The first snapshot freezes the schema and performs allocation and schema-registration callbacks. This happens before it is known whether the snapshot can be delivered. A no-sink attempt or failed initialization can leave the schema frozen despite producing no sample.

**Concrete change:** Add an explicit `prepare()` that validates and freezes the schema, allocates the configured capacities and registers sinks without emitting a sample. Give preparation a clear failure state and retry policy. Keep recording admission distinct from preparation; `startRecording()` alone is an ambiguous name for both.

**Timing:** **MUST 2.0** for the intended lifecycle semantics. The method is mechanically additive, but changing implicit first-snapshot behavior later is not semantically neutral.

### 29. `takeSnapshot()` needs a result that explains partial acceptance

**Locations:** `data_tamer_cpp/include/data_tamer/channel.hpp:188`, `data_tamer_cpp/include/data_tamer/channel.hpp:190`; `data_tamer_cpp/src/channel.cpp:417`, `data_tamer_cpp/src/channel.cpp:427`, `data_tamer_cpp/src/channel.cpp:476`.

`false` covers no sinks, pool exhaustion, strict-capacity rejection and failure to enqueue into at least one sink. Some sinks may already have accepted the same snapshot.

The method may also block on the write mutex or throw from allocation and serializers. None of this resembles an all-or-nothing “take a snapshot” operation.

**Concrete change:** Use a named result describing whether a record was captured and how many or which destinations accepted it, plus the rejection reason. Mark it `[[nodiscard]]`. Define a non-blocking `tryTakeSnapshot()` that requires successful preparation and does not grow buffers. Keep setup failures as exceptions if desired; define how serializer failures become RT-path results.

**Timing:** **MUST 2.0** for the replacement result and exception policy. The additional try method can wait if the contract is settled now.

### 30. The eight-sink cap is a policy choice, not inherently an ABI problem

**Locations:** `data_tamer_cpp/include/data_tamer/channel.hpp:164`; `data_tamer_cpp/src/channel.cpp:87`, `data_tamer_cpp/src/channel.cpp:214`.

The cap is hard-coded and attachment fails when it is exceeded. The public API provides no capacity query. The implementation does, however, keep the actual table inside the channel implementation.

**Concrete change:** Document the cap and failure behavior, expose a capacity query, and decide whether configuration before preparation is needed. Do not replace a small bounded table with an unbounded RT container merely to remove the number eight.

**Timing:** The documented contract should be settled in **2.0**. Raising the cap or introducing a prepared larger table **can wait**; relaxing this limit does not itself require an ABI break.

### 31. Statistics need coherent semantics and an extensible representation

**Locations:** `data_tamer_cpp/include/data_tamer/channel.hpp:222`, `data_tamer_cpp/include/data_tamer/channel.hpp:252`; `data_tamer_cpp/src/channel.cpp:294`; `data_tamer_cpp/include/data_tamer/data_sink.hpp:88`.

There are both individual getters and a public aggregate `Stats`. Returning that aggregate by value exposes its size and calling convention. Adding a field later can break ABI.

The counters describe different stages: channel contention, pool exhaustion, per-attachment enqueue loss and sink callback exceptions. The aggregate is not an atomic snapshot of all activity, and collecting it includes a controller lock.

**Concrete change:** Document units, lifetime, reset behavior, concurrency and whether collection is RT-safe. Use a stable stats object with accessors, or a versioned/size-tagged output structure if a plain layout is required. Add accepted, rejected and delivered/error counters with explicitly different meanings.

**Timing:** **MUST 2.0** for the representation and semantics. Additional counters can wait behind an extensible boundary.

### 32. Registry ownership and cleanup behavior need explicit limits

**Locations:** `data_tamer_cpp/include/data_tamer/data_tamer.hpp:1`; `data_tamer_cpp/src/data_tamer.cpp:27`, `data_tamer_cpp/src/data_tamer.cpp:33`, `data_tamer_cpp/src/data_tamer.cpp:50`.

`addDefaultSink()` stores null or excessive defaults without validating the future channel creation they will affect. Defaults apply only to subsequently created channels. `clear()` removes registry ownership, but externally held channels survive and can coexist with a new same-named channel.

Cleanup also destroys owned objects while holding the registry lock; a sink destructor waiting on work that needs that registry can deadlock.

**Concrete change:** Validate default sinks when adding them. Name the operation to make “future channels” clear, and provide removal if defaults are mutable. Define `clear()` as registry removal, not recording shutdown. Move potentially blocking destruction outside the registry lock.

**Timing:** **MUST 2.0** for semantics and validation. Additional lookup convenience APIs can wait.

### 33. Remove compatibility debris and test utilities from the supported surface

**Locations:** `data_tamer_cpp/include/data_tamer/data_sink.hpp:47`; `data_tamer_cpp/include/data_tamer/details/locked_reference.hpp:41`, `data_tamer_cpp/include/data_tamer/details/locked_reference.hpp:96`; `data_tamer_cpp/include/data_tamer_parser/data_tamer_parser.hpp:247`; `data_tamer_cpp/CMakeLists.txt:145`.

`DataSnapshot` retains documentation about a `SnapshotHeader` representation that no longer describes the current API. Deprecated mutex accessors and scalar pointer/writeback proxies preserve confusing behavior. `DummySink` is shipped as though it were a production sink, including the misleading ownership implications of copying `Snapshot`.

The parser’s public entry point is misspelled `BuilSchemaFromText`.

**Concrete change:** Remove obsolete aliases and deprecated scalar proxy APIs in 2.0. Move `DummySink` to test/example support. Rename the parser entry point to a consistent parse/build name; if migration requires an alias, make it a deliberately short-lived compatibility header. Historical wire decoding is different from obsolete source shims: retain the former.

**Timing:** **MUST 2.0** for removals and naming cleanup.

## C. ABI boundaries for the rest of 2.x

### 34. `LogChannel` is only partly behind Pimpl

**Locations:** `data_tamer_cpp/include/data_tamer/channel.hpp:266`; `data_tamer_cpp/include/data_tamer/custom_types.hpp:58`.

The channel stores `TypesRegistry` directly, outside its Pimpl. That registry embeds an unordered map and recursive mutex. Changing registration internals therefore changes `LogChannel` layout despite the apparent Pimpl boundary.

Private methods called by public templates are also part of the effective binary interface: already-compiled client template instantiations call them.

**Concrete change:** Move registry storage into the Pimpl. Keep template front ends thin and route them through deliberately stable non-template registration functions. Include those functions in ABI review even when they are private in C++.

**Timing:** **MUST 2.0**.

### 35. Public implementation headers pin the proposed hot-path redesigns

**Locations:** `data_tamer_cpp/include/data_tamer/details/shared_state.hpp:32`, `data_tamer_cpp/include/data_tamer/details/shared_state.hpp:145`; `data_tamer_cpp/include/data_tamer/details/write_mutex.hpp:33`; `data_tamer_cpp/include/data_tamer/details/snapshot_pool.hpp:20`, `data_tamer_cpp/include/data_tamer/details/snapshot_pool.hpp:112`; `data_tamer_cpp/include/data_tamer/values.hpp:28`.

`ChannelSharedState`, transactions, mutex storage, pool slots, retained references and type-erased value operations are defined inline in installed headers. Calling a directory `details` does not hide layouts that public templates or return types require.

This directly affects the proposals to replace the transaction chain, change pool allocation and remove `shared_ptr` from `SnapshotRef`.

**Concrete change:** Make retained-snapshot and transaction representations opaque, with out-of-line lifetime operations. Keep unavoidable typed value storage in templates, but route coordination through stable non-template operations. Stop exporting mutable pool and shared-state internals.

Do not add a heap allocation to every scalar write merely to obtain Pimpl purity.

**Timing:** **MUST 2.0** for the boundaries. The optimizations themselves can wait.

### 36. Removing `shared_ptr` from `SnapshotRef` requires a replacement ownership proof

**Locations:** `data_tamer_cpp/include/data_tamer/details/snapshot_pool.hpp:112`, `data_tamer_cpp/include/data_tamer/details/snapshot_pool.hpp:146`, `data_tamer_cpp/include/data_tamer/details/snapshot_pool.hpp:155`; `data_tamer_cpp/tests/snapshot_pool_tests.cpp:1`.

The shared owner keeps the pool, slots and channel-name storage alive after the channel has disappeared. Slot reference counts alone do not keep their containing pool alive.

**Concrete change:** First hide the handle representation. If measurement justifies eliminating the shared owner, replace it with an explicit pool/control-block lifetime mechanism that survives every retained slot. Preserve the existing after-channel-destruction tests.

**Timing:** Opaque representation **MUST 2.0**; ownership optimization **can wait**.

### 37. Establish which classes are extension points

**Locations:** `data_tamer_cpp/include/data_tamer/channel.hpp:47`, `data_tamer_cpp/include/data_tamer/channel.hpp:53`, `data_tamer_cpp/include/data_tamer/channel.hpp:60`; `data_tamer_cpp/include/data_tamer/data_sink.hpp:62`; `mcap_sink.hpp:16`.

`LogChannel` has a protected constructor and a non-virtual destructor, an awkward signal about inheritance. Concrete sinks are inheritable despite their lifecycle and Pimpl internals. `DataSinkBase` is a genuine extension point and therefore freezes a vtable contract.

**Concrete change:** Make `LogChannel` and concrete sinks `final` unless subclassing is intentionally supported. Put test seams in test support. Give the actual sink extension interface a documented versioning policy; do not casually add virtual methods during 2.x.

**Timing:** **MUST 2.0**.

### 38. State a realistic ABI promise instead of implying universal binary portability

**Locations:** `data_tamer_cpp/include/data_tamer/types.hpp:44`, `data_tamer_cpp/include/data_tamer/types.hpp:111`, `data_tamer_cpp/include/data_tamer/types.hpp:137`; `data_tamer_cpp/include/data_tamer/custom_types.hpp:12`; `data_tamer_cpp/include/data_tamer/data_sink.hpp:28`.

Public APIs expose `std::string`, vectors, maps, variants, shared pointers, chrono types and exceptions. Pimpl does not make those interoperable across arbitrary standard libraries, compiler ABIs or runtime configurations.

The public `VarNumber` variant and schema aggregates also freeze their alternatives, fields and layouts. Adding strings to the existing variant, for example, is not an ABI-neutral extension.

**Concrete change:** Promise ABI compatibility only within a defined toolchain/platform configuration. Keep schema objects immutable through accessors if they must evolve during 2.x. Use a new value-view API for future non-numeric values rather than silently expanding `VarNumber`. Add a C facade later if cross-language or cross-toolchain ABI is required.

**Timing:** **MUST 2.0** for the promise and extensible public types. A C facade can wait.

### 39. Compile-time policy constants and platform-specific mutex details are exposed

**Locations:** `data_tamer_cpp/include/data_tamer/data_sink.hpp:65`; `data_tamer_cpp/include/data_tamer/details/write_mutex.hpp:79`, `data_tamer_cpp/include/data_tamer/details/write_mutex.hpp:82`, `data_tamer_cpp/include/data_tamer/details/write_mutex.hpp:152`.

Defaults passed through header-defined default arguments are compiled into callers. The priority-inheritance flag describes a build path, not necessarily successful runtime availability. Mutex layout and spin behavior are also client-visible.

Some pthread operations ignore return codes; a failed operation must not be treated as successful ownership.

**Concrete change:** Put tunable defaults in library-owned overloads/options, expose a runtime capability query after initialization, and handle mutex initialization/operation failures according to a documented policy. Keep protocol constants such as schema version fixed and explicit; they are not runtime tuning knobs.

**Timing:** **MUST 2.0** for visibility and error policy. Tuning the private implementation can wait.

### 40. The header-only parser has a distribution compatibility problem, not a shared-library ABI solution

**Locations:** `data_tamer_cpp/include/data_tamer_parser/data_tamer_parser.hpp:20`, `data_tamer_cpp/include/data_tamer_parser/data_tamer_parser.hpp:247`; `python/data_tamer_parser.py:17`; `docs/wire_format.md:214`.

A copied parser does not receive fixes when the recording library is upgraded. Updating the writer cannot repair the parser embedded in an existing PlotJuggler installation.

The parser also owns its own copies of types and hashing logic, which can drift from the writer and Python implementation.

**Concrete change:** Version the parser artifact independently from the wire version. Publish readable-version capabilities and a migration matrix. Distribute a generated standalone header from one maintained source, with the same conformance corpus used by C++ and Python. Coordinate the parser update with the released downstream before emitting new wire versions.

**Timing:** **MUST 2.0** for the distribution and compatibility policy. Keep version-4 reading; do not conflate it with preserving obsolete writer APIs.

### 41. Build and packaging metadata are not ready for a 2.0 ABI release

**Locations:** `data_tamer_cpp/CMakeLists.txt:3`, `data_tamer_cpp/CMakeLists.txt:8`, `data_tamer_cpp/CMakeLists.txt:100`, `data_tamer_cpp/CMakeLists.txt:174`, `data_tamer_cpp/CMakeLists.txt:184`; `data_tamer_cpp/cmake/data_tamerConfig.cmake.in:3`; `data_tamer_cpp/conanfile.py:8`; `data_tamer_cpp/package.xml:1`; `data_tamer_msgs/package.xml:1`; `data_tamer_cpp/CHANGELOG.rst:1`.

The CMake, Conan and ROS package versions disagree and are not consistently 2.0.0. The version compile definition uses the top-level CMake project version, which can be the consuming project’s version when included as a subproject.

A package version file is generated but not installed alongside the config. The config uses an unsuitable version substitution and does not comprehensively rediscover transitive dependencies. There is no deliberate shared-library version/SOVERSION boundary, and Windows exports are broad rather than curated.

**Concrete change:** Use one release-version source, `PROJECT_VERSION` where appropriate, install a working ConfigVersion file, export complete dependency requirements and set the shared-library ABI version. Test installed-package consumption separately from in-tree builds, including static linking and use as a subproject.

**Timing:** **MUST 2.0**.

### 42. The clean build and portability claims need qualification

**Locations:** `data_tamer_cpp/3rdparty/mcap/include/mcap/types.hpp:18`; `data_tamer_cpp/tests/wire_format_tests.cpp:24`; `data_tamer_cpp/tests/alloc_counter.cpp:1`; `docs/benchmarks/2026-09-plan4.md:174`.

The fresh GCC 15 build failed because the vendored MCAP header uses fixed-width integer types without including `<cstdint>`. Building with `-include cstdint` bypassed the defect; that is a review workaround, not a release fix.

There are also Linux/compiler-specific assumptions in tests and allocation instrumentation. Existing Linux validation does not establish Windows/macOS support, and the schema hash parsing already contains a concrete Windows-width defect.

**Concrete change:** Fix or update the vendored dependency correctly. Build public headers independently to catch missing includes. Add native Windows/macOS jobs and distinguish “unsupported,” “compiles,” and “tested” platforms in the README.

**Timing:** Clean builds on claimed platforms **MUST 2.0**. Expanding the platform matrix can wait if claims are narrowed explicitly.

## D. Wire-format and schema decisions

### 43. Separate logical stream identity, schema identity and schema version

**Locations:** `data_tamer_cpp/include/data_tamer/types.hpp:137`; `data_tamer_cpp/src/types.cpp:149`; `data_tamer_cpp/include/data_tamer_parser/data_tamer_parser.hpp:80`; `data_tamer_msgs/msg/Schema.msg:1`; `docs/wire_format.md:214`.

The hash includes the channel name and describes one exact ordered schema. Parser schema objects do not retain the parsed version. This makes it difficult to distinguish “same stream, changed schema” from “different stream, same value structure,” and complicates legacy handling.

**Concrete change:** Retain the wire version in parsed schemas. Define schema identity as an exact immutable description, with a separate logical stream identity. Preserve old schemas while records referring to them remain possible. Do not use a hash alone as an evolution policy.

**Timing:** **MUST 2.0** for public identities and version access. Dynamic evolution support can wait.

### 44. Field names need a reversible path model

**Locations:** `data_tamer_cpp/src/channel.cpp:129`; `data_tamer_cpp/include/data_tamer_parser/data_tamer_parser.hpp:417`; `python/data_tamer_parser.py:129`; `docs/wire_format.md:110`.

Registration only rejects the ordinary space character. It does not consistently reject newlines, reserved syntax or ambiguous path/index characters. Parsers flatten nested names with `/` and `[index]`; distinct structures can therefore produce the same output name. Python’s dictionary can silently overwrite one value with another.

This also blocks reliable automatic protobuf nesting by `/`.

**Concrete change:** Define whether a registration name is a literal label or a path. Use explicit segments or a documented escaping scheme. Reject ambiguous names and scalar/container path conflicts during preparation. Keep display labels separate if arbitrary user-facing text is needed.

**Timing:** **MUST 2.0**. Tightening names after users have recorded them creates migration problems.

### 45. Strings and blobs need distinct types and explicit bounds

**Locations:** `data_tamer_cpp/include/data_tamer/types.hpp:20`, `data_tamer_cpp/include/data_tamer/types.hpp:44`; `data_tamer_cpp/include/data_tamer/contrib/SerializeMe.hpp:69`, `data_tamer_cpp/include/data_tamer/contrib/SerializeMe.hpp:520`; `docs/wire_format.md:46`.

The wire schema has no string primitive. The serialization helper’s string encoding uses a 16-bit length, while sequences use a 32-bit count. Exposing that helper as a new library feature would accidentally choose an incompatible length and semantic model.

Text and arbitrary bytes also need different treatment by visualization tools.

**Concrete change:** Specify UTF-8 text separately from opaque bytes, including maximum length, empty-versus-disabled semantics and malformed-text handling. Introduce a value-view extension that does not require changing the numerical variant. For bounded recording, distinguish maximum capacity from current length.

**Timing:** Type-system and evolution decisions **MUST 2.0**. Actual string/blob recording **can wait** behind an explicit new wire encoding/version.

### 46. Preserve enum numbers; add names as schema metadata

**Locations:** `data_tamer_cpp/include/data_tamer/types.hpp:65`; `docs/wire_format.md:69`.

Enums currently serialize as their underlying integer type. This loses the mapping from values to names, but replacing integers with names would lose unknown values, aliases and efficient numerical handling.

**Concrete change:** Keep the integer wire value and attach an optional enum table to the schema. Define aliases, flags and unknown values. Preserve the underlying signedness and width. Do not make successful decoding depend on the enum table containing every runtime value.

**Timing:** Metadata extensibility **MUST 2.0**; enum-table support **can wait** if it can be added without changing existing layouts or field encodings.

### 47. Top-level masking and nested-field presence must not be conflated

**Locations:** `data_tamer_cpp/src/channel.cpp:51`; `data_tamer_cpp/include/data_tamer_parser/data_tamer_parser.hpp:466`; `docs/wire_format.md:121`.

Only top-level registered fields are independently maskable. A reflected structure is serialized as one active unit. Adding per-nested-field bits later changes the mask interpretation and payload traversal.

**Concrete change:** Document the present top-level guarantee precisely. If independent nested presence is a real target, define a new explicit presence layout or flatten the schema during registration. Do not silently reinterpret the existing mask. Preserve disabled versus enabled-empty sequences.

**Timing:** The extension/versioning decision **MUST 2.0**. Nested masking itself **can wait** as an opt-in format capability.

### 48. Opaque custom encodings are only partially implemented

**Locations:** `data_tamer_cpp/include/data_tamer/custom_types.hpp:21`; `data_tamer_cpp/src/types.cpp:170`; `data_tamer_cpp/include/data_tamer_parser/data_tamer_parser.hpp:453`; `python/data_tamer_parser.py:99`; `docs/wire_format.md:122`.

The format permits one opaque schema section that consumes the remainder of the text, but the writer can emit multiple entries. The C++ parser does not implement the advertised custom callback; the callback argument is unused. It also lacks a general way to determine and skip an arbitrary opaque field’s bytes.

**Concrete change:** Immediately reject unsupported multiple opaque sections. Either implement a complete custom decoder contract with explicit framing and schema ownership, or remove the misleading callback/API until that contract exists. For a future format, use length-delimited opaque values and structurally delimited schema sections.

**Timing:** **MUST 2.0** to stop emitting or advertising unsupported behavior. General opaque codecs can wait behind a new format capability.

### 49. Timestamp representation is not a clock contract

**Locations:** `data_tamer_cpp/include/data_tamer/channel.hpp:18`, `data_tamer_cpp/include/data_tamer/channel.hpp:190`; `data_tamer_cpp/include/data_tamer/data_sink.hpp:36`; `mcap_sink.cpp:139`; `data_tamer_msgs/msg/Snapshot.msg:1`; `docs/wire_format.md:175`.

The API accepts nanoseconds, defaults to `system_clock`, and does not identify the clock domain. A caller can supply simulation time, a steady-clock epoch or device ticks converted to nanoseconds. Sinks cannot tell which it is. Negative values can become very large unsigned timestamps.

MCAP log and publish times are assigned the same value, and the sequence number is always one.

**Concrete change:** Introduce an explicit timestamp/clock-domain contract. Distinguish capture time from recording time, define negative values and clock resets, and assign a meaningful sequence number. Preserve an inexpensive caller-supplied timestamp path.

**Timing:** **MUST 2.0** for public timestamp semantics and any changed ROS message fields. Clock synchronization facilities can wait.

### 50. Protobuf can be first-class without becoming the producer’s hot-path representation

**Locations:** `data_tamer_cpp/include/data_tamer/data_sink.hpp:98`; `mcap_sink.cpp:119`; `data_tamer_cpp/include/data_tamer/types.hpp:111`; `docs/wire_format.md:188`.

The sink thread is already the appropriate boundary for transcoding. Making protobuf an optional MCAP output mode does not require changing how numerical values are copied into the current snapshot pool.

The proposed presence mapping needs one correction: proto2 repeated fields do **not** distinguish absent from present-but-empty. An optional wrapper message containing the repeated field is needed when that distinction matters. Proto2 enum APIs also need deliberate treatment of unknown numeric values. [Protobuf field-presence documentation](https://protobuf.dev/programming-guides/field_presence/).

**Concrete change:** Define an opt-in output encoding option and a deterministic schema-to-descriptor mapping. Give fields stable numbers, detect name/path conflicts, preserve original names, and wrap sequences where presence must survive. Store the standard descriptor-set representation required by MCAP’s protobuf encoding. [MCAP format registry](https://mcap.dev/spec/registry).

**Timing:** Output options and schema metadata boundaries **MUST 2.0**. The protobuf implementation **can wait**; it is an additive sink capability.

## E. What the current verification establishes—and does not

A fresh non-ROS Debug build with GCC 15 required the `<cstdint>` compiler workaround described above. CTest discovered 133 tests and reported no failures, with one priority-inheritance test skipped. That does not establish ROS behavior, Windows/macOS compatibility or sanitizer cleanliness for this review. The defects above are source findings unless explicitly described as a build/test result.

The following gaps should be release gates:

- **Malformed-input coverage:** bounds, excessive counts, cyclic schemas, invalid primitive tokens, opaque schema contents and cross-language disagreement. Current conformance tests concentrate on valid fixtures. References: `data_tamer_cpp/tests/wire_format_tests.cpp:1`, `python/test_data_tamer_parser.py:1`.
- **Registration failure coverage:** throwing schema discovery, failed construction, same-name conflicting types and serializer replacement. References: `data_tamer_cpp/src/channel.cpp:124`, `data_tamer_cpp/tests/channel_control_tests.cpp:313`.
- **Transaction coverage:** enable/disable changes within transactions and combinations of transaction and pointer guards. Reference: `data_tamer_cpp/tests/transaction_tests.cpp:270`.
- **Delivery coverage:** write failure, finalization failure, destruction with queued records, failed restart, same-named schema generations and ROS late joining. References: `data_tamer_cpp/tests/sink_queue_tests.cpp:327`, `data_tamer_cpp/tests/ros2_publisher_tests.cpp:1`.
- **Actual ABI coverage:** compiling an old consumer and running/linking it against a newer library. Size assertions for Pimpl sinks do not detect changed vtables, public aggregate layouts or inline/template dependencies. Reference: `data_tamer_cpp/tests/abi_tests.cpp:1`.
- **Meaningful performance coverage:** record accepted and delivered counts alongside latency. The microbenchmark ignores the snapshot result, and `DT_Doubles` registers one vector rather than a thousand independent fields. Fast pool-exhaustion rejection must not be counted as successful snapshot throughput. References: `data_tamer_cpp/benchmarks/data_tamer_benchmark.cpp:20`, `data_tamer_cpp/benchmarks/data_tamer_benchmark.cpp:33`; `docs/benchmarks/2026-09-12-main-vs-lockfree-frontend.md:25`.

The benchmark documentation also records frequent queue wakeups and shared-pool memory costs. “No producer allocations after preparation” is a narrower claim than “wait-free,” “no syscalls,” or “bounded worst-case execution.” Those claims need separate evidence. References: `docs/benchmarks/2026-09-plan3.md:168`, `docs/benchmarks/2026-09-plan4.md:21`.

# Part 2 — Ranked roadmap

Costs are relative engineering and validation effort: **S** is localized work, **M** spans several components, **L** introduces a substantial capability, and **XL** creates a materially different operating profile. Ranking assumes the correctness blockers in Part 1 are fixed first.

## 1. Explicit preparation and a bounded non-blocking snapshot API

**Value:** The clearest improvement for control-loop users: predictable startup, no accidental first-sample allocation, and an observable reason when recording cannot proceed.

**Concrete scope:** `prepare()`, capacity validation, `tryTakeSnapshot()`, explicit capture/admission results, and documented serialization requirements. Preserve a convenience blocking API for non-RT callers.

**Cost:** **M**, dominated by lifecycle semantics and concurrency tests.

**Code basis:** `data_tamer_cpp/src/channel.cpp:382`, `data_tamer_cpp/src/channel.cpp:427`, `data_tamer_cpp/src/channel.cpp:441`; `data_tamer_cpp/include/data_tamer/channel.hpp:230`.

**2.0 dependency:** Opaque transaction/state boundaries, stable options and result types, settled exception policy.

**Breaking-change flag:** **Anticipate in 2.0.** Adding the try method is compatible; changing existing startup and result semantics later is not.

## 2. Standard protobuf MCAP output, followed by JSON-schema output

**Value:** Makes recordings readable by tools that understand standard encodings without requiring a Data Tamer-specific parser. Foxglove supports standard custom-data encodings including protobuf and JSON. [Foxglove custom-data documentation](https://docs.foxglove.dev/docs/getting-started/custom).

**Concrete scope:** Build descriptors at preparation/schema-registration time; transcode on the sink thread; keep the current compact encoding available. Add JSON output as a debugging and interchange option, with explicit policies for 64-bit integers, non-finite floats, byte strings and absent values.

**Cost:** **M–L** for protobuf; **M** for a faithful JSON mapping.

**Code basis:** `mcap_sink.cpp:119`; `data_tamer_cpp/include/data_tamer/custom_types.hpp:12`; `data_tamer_cpp/include/data_tamer/types.hpp:111`.

**2.0 dependency:** Reversible names, stable field identity, complete presence semantics and extensible output options. Repeated-field wrappers are required to preserve enabled-empty arrays; field numbers must not be casually reassigned when fields move.

**Breaking-change flag:** **No producer ABI break required** if these boundaries exist. Both formats can be additive opt-in outputs.

## 3. One authoritative schema model and a cross-language conformance corpus

**Value:** Prevents the writer, copied PlotJuggler parser and Python decoder from interpreting the same recording differently.

**Concrete scope:** A validated immutable schema model; shared fixtures for every primitive and shape; malformed cases; version-4/version-5 compatibility tests; differential decoding; fuzz targets; a versioned standalone parser release.

**Cost:** **M**, with continuing maintenance.

**Code basis:** `data_tamer_cpp/src/types.cpp:96`; `data_tamer_cpp/include/data_tamer_parser/data_tamer_parser.hpp:247`; `python/data_tamer_parser.py:75`; `docs/wire_format.md:214`.

**2.0 dependency:** Exact grammar, canonical hashing, parser error contract and explicit readable versions.

**Breaking-change flag:** **Parser contract changes belong in 2.0.** Expanding the corpus and distributing fixes do not require later ABI breaks.

## 4. Trustworthy recording lifecycle, diagnostics and bounded retention

**Value:** Users can tell whether data reached disk and can stop or rotate a recording without guessing how much is lost.

**Concrete scope:** Check write/finalization results; explicit flush/close; preserve files by default; rotate to new names; configurable retention by bytes/time/file count; distinguish enqueue loss from storage failure.

**Cost:** **M**.

**Code basis:** `data_tamer_cpp/src/data_sink.cpp:32`; `mcap_sink.cpp:52`, `mcap_sink.cpp:145`, `mcap_sink.cpp:184`, `mcap_sink.cpp:201`.

**2.0 dependency:** Sink lifecycle, delivery result, error-reporting and open-mode decisions.

**Breaking-change flag:** **Settle semantics in 2.0.** Additional retention policies can be compatible options.

## 5. Installable, dependency-light packages with reliable versioning

**Value:** Makes the library practical outside the original workspace and avoids forcing ROS or MCAP dependencies onto users who need only the numerical recorder or parser.

**Concrete scope:** Separate core, MCAP and ROS build targets; a standalone parser target/package; consistent CMake/ROS/Conan versions; complete install/export tests; reproducible example environments through pixi or equivalent lockfiles. Ensure packaged sources contain the fixtures needed by enabled tests.

**Cost:** **M**.

**Code basis:** `data_tamer_cpp/CMakeLists.txt:62`, `data_tamer_cpp/CMakeLists.txt:123`, `data_tamer_cpp/CMakeLists.txt:145`, `data_tamer_cpp/CMakeLists.txt:184`; `data_tamer_cpp/conanfile.py:8`.

**2.0 dependency:** Stable target names, exported dependency policy and ABI version.

**Breaking-change flag:** **Choose package/target boundaries in 2.0.** Additional package-manager recipes can wait without breaking the C++ API.

## 6. Explicit external clocks, capture time and sequence identity

**Value:** Supports simulation, hardware timestamps, distributed robots and recordings spanning clock corrections. It also makes dropped-record diagnosis possible.

**Concrete scope:** Caller-supplied capture timestamps with a declared domain; separate recording time; monotonic sequence IDs; clock-reset markers; optional clock correlation records.

**Cost:** **M** initially, **L** for distributed synchronization tooling.

**Code basis:** `data_tamer_cpp/include/data_tamer/channel.hpp:18`, `data_tamer_cpp/include/data_tamer/channel.hpp:190`; `mcap_sink.cpp:139`; `data_tamer_msgs/msg/Snapshot.msg:1`.

**2.0 dependency:** Timestamp type and message-envelope extensibility.

**Breaking-change flag:** **Anticipate in 2.0**, especially ROS message changes. Synchronization adapters can be added later.

## 7. Bounded strings and blobs with honest ownership semantics

**Value:** Numerical telemetry often needs accompanying state names, diagnostic messages, IDs and small binary payloads. These should not require unsafe custom serializers.

**Concrete scope:** Distinct UTF-8 and byte-sequence types; explicit capacities; owned-copy and borrowed-input registration; immutable retained output. For larger payloads, support an owner-backed external buffer only when its lifetime and immutability can be guaranteed.

**Cost:** **L**.

**Code basis:** `data_tamer_cpp/include/data_tamer/types.hpp:20`; `data_tamer_cpp/include/data_tamer/contrib/SerializeMe.hpp:69`; `data_tamer_cpp/include/data_tamer/details/snapshot_pool.hpp:112`; `docs/wire_format.md:46`.

**2.0 dependency:** Extensible value/schema APIs, length framing and separate snapshot view/owner types.

**Breaking-change flag:** **Anticipate in 2.0; implementation can wait.** True zero-copy input is an ownership feature, not merely accepting `string_view`.

## 8. Units, enum names, frames and field descriptions

**Value:** Turns anonymous numbers into interpretable robotics data: radians versus degrees, device frame versus world frame, enum state names, vector dimensions and meaningful descriptions.

**Concrete scope:** Optional per-field metadata for units, enum tables, descriptions, coordinate frames and semantic roles. Keep numeric values intact. Do not hard-code a particular viewer’s property model into the producer.

**Cost:** **M**.

**Code basis:** `data_tamer_cpp/include/data_tamer/types.hpp:111`, `data_tamer_cpp/include/data_tamer/types.hpp:137`; `docs/wire_format.md:69`.

**2.0 dependency:** Schema extensibility and immutable metadata access.

**Breaking-change flag:** **Anticipate in 2.0.** Appending members to the current public schema aggregates later would break ABI.

## 9. First-class event and one-shot records

**Value:** Periodic snapshots miss short-lived transitions and make sparse events awkward. Fault transitions, task milestones and annotations should be recorded when they happen.

**Concrete scope:** An event channel with a prepared schema and `record(...)` operation that captures values immediately. Reuse sink transport, timestamps, capacity accounting and output encodings. Do not register a pointer to a temporary and hope the next snapshot arrives in time.

**Cost:** **M–L**.

**Code basis:** `data_tamer_cpp/include/data_tamer/channel.hpp:80`, `data_tamer_cpp/include/data_tamer/channel.hpp:190`; `data_tamer_cpp/src/channel.cpp:453`.

**2.0 dependency:** A record envelope that does not equate all records with periodically sampled registrations; stable timestamp and ownership types.

**Breaking-change flag:** **Can be additive.** Avoid baking periodic-only assumptions into the revised sink API.

## 10. Multi-rate recording using explicit channel groups

**Value:** Records fast control values frequently and slow diagnostics economically, without forcing every value into one rate.

**Concrete scope:** Begin with helpers that coordinate existing channels and apply a common caller-supplied timestamp. Add per-group schedules only when needed. Specify that equal timestamps do not, by themselves, make separate channels atomically consistent.

**Cost:** **S–M** for channel-group helpers; **L** for an integrated scheduler.

**Code basis:** `data_tamer_cpp/include/data_tamer/data_tamer.hpp:1`; `data_tamer_cpp/include/data_tamer/channel.hpp:190`, `data_tamer_cpp/include/data_tamer/channel.hpp:217`.

**2.0 dependency:** Clear channel identity, transaction scope and timestamp semantics.

**Breaking-change flag:** **No breaking change required** for the initial implementation. Prefer existing channels before adding per-field scheduler state.

## 11. Schema evolution through explicit generations

**Value:** Long-running systems can add diagnostics or change modules without discarding the whole recording architecture.

**Concrete scope:** Retain immutable schema generations; announce a new generation before its records; preserve stable logical stream identity; define rename, removal and type-change behavior. Do not mutate the meaning of an existing schema hash.

**Cost:** **L**.

**Code basis:** `data_tamer_cpp/src/channel.cpp:134`, `data_tamer_cpp/src/channel.cpp:382`; `mcap_sink.cpp:45`; `data_tamer_cpp/include/data_tamer/types.hpp:137`.

**2.0 dependency:** Separate stream/schema identities, generation-safe registrations, version-aware parsers and extensible sink catalog APIs.

**Breaking-change flag:** **Anticipate in 2.0.** Evolution can later be implemented by replacing prepared generations rather than unfreezing existing ones.

## 12. Python and Rust producer APIs

**Value:** Extends the same recording and visualization workflow to Python robotics tooling, Rust components and mixed-language systems.

**Concrete scope:** Package the Python decoder independently. Add producer bindings around owned values, explicit transactions and prepared channels. For Rust, provide an ownership-safe API rather than exposing arbitrary borrowed pointers through FFI. Preserve exact integer types.

**Cost:** **M** for Python; **M–L** for Rust plus a stable C boundary.

**Code basis:** `python/data_tamer_parser.py:1`; `data_tamer_cpp/include/data_tamer/channel.hpp:80`, `data_tamer_cpp/include/data_tamer/channel.hpp:145`; `data_tamer_cpp/include/data_tamer/types.hpp:44`.

**2.0 dependency:** Unambiguous ownership, explicit unregister behavior, opaque handles and a coherent error model.

**Breaking-change flag:** **Bindings can be additive.** Do not expose STL layouts as the language-neutral ABI.

## 13. Bounded live streaming through an existing protocol

**Value:** Supports remote inspection without requiring every deployment to run ROS 2 or write a local file first.

**Concrete scope:** A streaming sink using a supported Foxglove/WebSocket integration, standard schemas and bounded per-client queues. Define reconnect behavior, schema replay, slow-client dropping and connection diagnostics. Avoid inventing another telemetry protocol. Foxglove documents custom data over both MCAP and WebSocket sources. [Foxglove custom-data documentation](https://docs.foxglove.dev/docs/getting-started/custom).

**Cost:** **L**.

**Code basis:** `data_tamer_cpp/include/data_tamer/data_sink.hpp:62`; `data_tamer_cpp/src/data_sink.cpp:106`; `data_tamer_msgs/msg/Schemas.msg:1`.

**2.0 dependency:** Retained-record ownership, stream/schema identities, delivery diagnostics and bounded backpressure policy.

**Breaking-change flag:** **No core ABI break required** after the sink boundary is corrected.

## 14. Isolation from slow or retaining sinks

**Value:** One viewer or custom sink should not exhaust the shared channel pool and prevent every other sink from receiving samples.

**Concrete scope:** Measure pool occupancy and retained references; document per-sink retention limits; optionally enforce retention/admission quotas. Offer an explicit copying boundary for sinks requiring long retention.

**Cost:** **M–L**.

**Code basis:** `data_tamer_cpp/include/data_tamer/details/snapshot_pool.hpp:55`; `data_tamer_cpp/src/channel.cpp:427`; `docs/benchmarks/2026-09-plan4.md:105`.

**2.0 dependency:** Opaque retained handles and explicit retention ownership. Pool size and queue size must remain distinct concepts.

**Breaking-change flag:** **Usually additive internally.** Introducing mandatory retention limits later changes behavior, so expose the policy model now.

## 15. Configurable compression and storage tradeoffs

**Value:** Different robots need different balances among CPU, storage bandwidth, disk space and recovery granularity.

**Concrete scope:** Replace the compression boolean with an extensible codec/options object; expose chunk sizing and compression level; benchmark accepted producer load and sink backlog separately. Keep compression on the sink thread.

**Cost:** **S–M** for options supported by the existing MCAP dependency; more for new codecs.

**Code basis:** `mcap_sink.hpp:28`, `mcap_sink.hpp:64`; `mcap_sink.cpp:70`, `mcap_sink.cpp:119`.

**2.0 dependency:** Stable options API and observable sink throughput/failure counters.

**Breaking-change flag:** **Replace the boolean in 2.0.** Additional codecs and tuning options can arrive later.

## 16. Headless replay, inspection and transcoding

**Value:** Users can validate a recording, extract selected signals, replay to a sink or convert to standard encodings without opening a GUI.

**Concrete scope:** Reuse the validated parser and schema model in a small CLI/library. Support field/time filtering, timestamp preservation, explicit real-time versus fastest-possible replay, and corruption diagnostics. Reuse MCAP’s existing capabilities for container-level operations.

**Cost:** **M–L**.

**Code basis:** `data_tamer_cpp/include/data_tamer_parser/data_tamer_parser.hpp:453`; `python/data_tamer_parser.py:160`; `data_tamer_cpp/examples/mcap_reader.cpp:1`.

**2.0 dependency:** Safe parsing, complete custom-encoding behavior and a record-view sink boundary.

**Breaking-change flag:** **No breaking change required.**

## 17. Tested PlotJuggler, Foxglove and Rerun integration layers

**Value:** Makes “record once, inspect where useful” a verified workflow rather than a claim based on file compatibility.

**Concrete scope:** Maintain a PlotJuggler parser compatibility fixture; validate standard MCAP output in Foxglove; build a Rerun adapter when there is a concrete workflow to test. Define naming, units, integer precision, arrays and disabled-value behavior in each adapter.

**Cost:** **M**, plus continuing compatibility maintenance.

**Code basis:** `data_tamer_cpp/include/data_tamer_parser/data_tamer_parser.hpp:44`, `data_tamer_cpp/include/data_tamer_parser/data_tamer_parser.hpp:417`; `docs/wire_format.md:188`.

**2.0 dependency:** Reversible paths, metadata, exact numeric values and stable schema identities.

**Breaking-change flag:** **No core ABI break required.** Viewer adapters should absorb viewer-specific conventions.

## 18. Native Windows, macOS and architecture-width testing

**Value:** Enables desktop tooling and mixed deployment environments while exposing assumptions currently hidden by Linux/x86-64 testing.

**Concrete scope:** Native builds and installed-package tests; 32-bit compile coverage for wire types where feasible; LLP64 hash parsing; filesystem and thread behavior; platform-specific PI capability reporting.

**Cost:** **M**, potentially **L** if every optional component must work everywhere.

**Code basis:** `data_tamer_cpp/include/data_tamer/details/write_mutex.hpp:11`; `data_tamer_cpp/include/data_tamer_parser/data_tamer_parser.hpp:323`; `data_tamer_cpp/CMakeLists.txt:8`; `docs/benchmarks/2026-09-plan4.md:174`.

**2.0 dependency:** Fixed-width protocol types, hidden synchronization layout and an explicit support matrix.

**Breaking-change flag:** **No intended API break.** Width and layout corrections should happen before 2.0.

## 19. Measured hot-path simplification

**Value:** Reduces per-sample CPU and tail latency once correctness and successful-delivery accounting are reliable.

**Concrete scope, in order:**

1. Measure a prepared channel containing many independently registered active fields.
2. Evaluate a packed active-field array that avoids separate size/write indirection where fixed sizes permit it.
3. Compare the pool scan with a free-slot bitmap under realistic occupancy.
4. Measure retained-handle ownership costs before replacing `shared_ptr`.
5. Compare the transaction chain with a recursive PI mutex under realistic nesting and contention.

**Cost:** **M**, with uncertain benefit until measured.

**Code basis:** `data_tamer_cpp/src/channel.cpp:70`, `data_tamer_cpp/src/channel.cpp:453`; `data_tamer_cpp/include/data_tamer/details/snapshot_pool.hpp:55`; `data_tamer_cpp/include/data_tamer/details/shared_state.hpp:32`; `docs/benchmarks/2026-09-12-main-vs-lockfree-frontend.md:39`.

**2.0 dependency:** Hide field dispatch, pool layout, retained-reference ownership and transaction implementation.

**Breaking-change flag:** **These should become internal 2.x changes.** Their current public representations are the reason to act in 2.0.

## 20. A deliberately restricted embedded/no-heap profile

**Value:** Supports smaller systems that need fixed memory use and cannot afford a thread, general-purpose containers or large optional dependencies.

**Concrete scope:** Start by separating “no allocation after preparation” from “no allocation ever.” For the latter, consider a distinct fixed-capacity recorder with caller-provided storage, bounded types and an externally driven drain operation. Compile out ROS, MCAP and unsupported dynamic types.

Do not force the unrestricted recorder to carry a universal allocator/backend abstraction before there is a tested embedded target.

**Cost:** **XL** for a credible no-heap profile; **M** for improving the existing prepared no-growth profile.

**Code basis:** `data_tamer_cpp/include/data_tamer/details/snapshot_pool.hpp:30`; `data_tamer_cpp/include/data_tamer/details/shared_state.hpp:145`; `data_tamer_cpp/src/data_sink.cpp:60`; `data_tamer_cpp/CMakeLists.txt:62`.

**2.0 dependency:** Separate core from sinks, avoid public dependence on a worker thread, and state allocation guarantees precisely.

**Breaking-change flag:** **A separate restricted API can be additive.** Making the current unrestricted API allocator-pluggable after promising its layouts would be much harder.