# Data Tamer wire format (schema version 5)

This document is the normative description of the bytes Data Tamer produces.
Anyone can implement a decoder in any language from it without reading the C++
sources. Two artifacts keep it honest:

- `docs/wire_format/vectors/` holds golden fixtures: a schema text, two
  snapshots (all fields enabled, two fields disabled), their MCAP message bodies
  and the decoded values (`expected.json`).
- `data_tamer_cpp/tests/wire_format_tests.cpp` regenerates those bytes from the
  library and fails if they differ. `python/data_tamer_parser.py` is a
  standard-library-only reference decoder and `python/test_data_tamer_parser.py`
  decodes the same fixtures and checks them against `expected.json`.

A change to any byte described here is a format revision: bump `SCHEMA_VERSION`
in `data_tamer/types.hpp`, `data_tamer_parser/data_tamer_parser.hpp` and
`python/data_tamer_parser.py` (and the version history below), regenerate the vectors with
`DATA_TAMER_UPDATE_GOLDEN=1 datatamer_test --gtest_filter='WireFormat.*'`,
update `expected.json` by hand (it is the oracle, never generated), update the
Python decoder, and describe the change in this file.

### Version history

| Version | Change |
|---:|---|
| 5 | Hash defined as FNV-1a 64 of the schema text; covers custom type bodies. |
| 4 | Lower-case type names, `MSG:` sections. Hash was `std::hash` based. |

## 1. Data model

A **channel** has a name and a **schema**: an ordered list of top-level
**fields**. A **snapshot** is one sample of all enabled fields of a channel at
one timestamp. Sinks receive `(timestamp, schema hash, active mask, payload)`.

A field has a type, a name and an optional container shape.

| Kind | Schema text | Wire encoding |
|---|---|---|
| basic scalar | `float64 speed` | the scalar, see table below |
| dynamic vector | `float64[] samples` | `uint32` count, then that many elements |
| fixed array | `int32[4] ids` | exactly N elements, no count |
| custom struct | `Pose pose` | its fields, in `MSG:` order, concatenated |
| vector of structs | `Point3D[] pts` | `uint32` count, then each struct |
| array of structs | `Pose[3] poses` | 3 structs, no count |

There are no strings, no alignment padding, no separators and no per-field tags.
All multi-byte numbers are **little-endian**.

### Basic types

The type id is the position in this table; it is used by the schema hash only,
never on the wire.

| id | name | bytes | encoding |
|---:|---|---:|---|
| 0 | `bool` | 1 | `0x00` false, `0x01` true |
| 1 | `char` | 1 | one byte, no charset implied |
| 2 | `int8` | 1 | two's complement |
| 3 | `uint8` | 1 | |
| 4 | `int16` | 2 | little-endian two's complement |
| 5 | `uint16` | 2 | little-endian |
| 6 | `int32` | 4 | little-endian two's complement |
| 7 | `uint32` | 4 | little-endian |
| 8 | `int64` | 8 | little-endian two's complement |
| 9 | `uint64` | 8 | little-endian |
| 10 | `float32` | 4 | IEEE 754 binary32, little-endian |
| 11 | `float64` | 8 | IEEE 754 binary64, little-endian |

Type id 12, `other`, denotes a custom struct named in the schema. Enums are
recorded as their underlying integer type; the schema does not preserve the
enum name.

## 2. Schema text

The schema is UTF-8 text, one item per line, `\n` terminated. Decoders must
trim spaces and `\r` at both ends of a line and skip empty lines.

```
### version: 5
### hash: <uint64>
### channel_name: wire_test

bool flag
float64[] vec
int32[4] arr
Pose pose
Point3D[] points
===========================================================
MSG: Point3D
float64 x
float64 y
float64 z
===========================================================
MSG: Pose
Point3D position
uint32 stamp
```

(Abridged: the complete text, with its hash, is
`docs/wire_format/vectors/schema.txt`.)

Grammar, in the order lines appear:

1. `### version: <int>` – 5 for texts written by this version. Decoders must
   also accept 4, which differs only in how the hash was computed (see section
   5), and reject anything else.
2. `### hash: <uint64>` – the schema hash, see section 5.
3. `### channel_name: <text>` – everything after the first space following the
   colon, trimmed. May contain spaces.
4. Zero or more **field lines**: `<type-spec> <name>`. Exactly one space
   separates the two; the name is everything after it (trimmed). A name never
   contains a space. `type-spec` is a basic type name or a custom type name,
   optionally followed by `[]` (dynamic vector) or `[N]` (fixed array, decimal
   N, 1 to 65535). Top-level field order is the mask bit order and the payload
   order.
5. Zero or more **custom type sections**. Each starts with a line consisting of
   `=` characters (at least 30; the writer emits 59), then `MSG: <TypeName>`,
   then field lines with the same grammar as above. Sections are emitted sorted
   by type name; decoders must not depend on that, since a type may reference
   another type declared later in the text. Nested fields are not individually
   maskable.
6. Optionally, **one opaque custom encoding**: a section whose `MSG:` line is
   followed by `ENCODING: <name>` and then the foreign schema text. The writer
   emits it after every ordinary section and it owns the rest of the text,
   because a foreign schema may itself contain `=====` and `MSG:` lines; for
   that reason a schema with more than one opaque type is not decodable and
   producers must not emit one. The payload bytes of such a field are produced
   by user code; this document does not define them and generic decoders
   cannot skip them. Producers that need generic decoding must not use them.

Legacy files (version < 4, before 2023) used upper-case type names (`DOUBLE`,
`INT32`, ...) with the name first; the C++ parser still accepts them, new
decoders may ignore that.

## 3. Snapshot

### 3.1 Active mask

At least `ceil(F / 8)` bytes for `F` top-level fields; a shorter mask is a
malformed snapshot and decoders must reject it. Bit `i` of the mask is
`mask[i >> 3] & (1 << (i & 7))`: byte 0 holds fields 0 to 7, the least
significant bit first. A set bit means the field is present in the payload.
Bits beyond `F - 1` are unspecified (the writer currently sets them); ignore
them. Disabling a field clears its bit and removes its bytes from the payload;
nothing else moves.

### 3.2 Payload

The concatenation, in schema order, of every top-level field whose mask bit is
set, each encoded per section 1. Decoding is a single forward pass:

```
pos = 0
for i, field in enumerate(schema.fields):
    if bit(mask, i):
        decode_field(field)          # advances pos

decode_field(field):
    count = field.array_size
    if field.is_vector and count == 0:
        count = read uint32           # dynamic vector length prefix
    repeat (1 if not field.is_vector else count) times:
        if field.type is basic:  read that many bytes, little-endian
        else:                    for sub in custom_types[field.type]: decode_field(sub)
```

After the loop `pos` must equal the payload length; otherwise the schema and
the payload do not belong together. Decoders must check every read against the
remaining payload before performing it, reject a dynamic count larger than the
remaining bytes allow, and bound the nesting depth of custom types (the reference
decoders use 64) so that a malformed or cyclic schema cannot recurse forever.

Flattened series names, as produced by the reference decoders and PlotJuggler:
nested fields join with `/`, container elements append `[i]`:
`pose/position/x`, `vec[2]`, `points[1]/z`.

### 3.3 Timestamp

Nanoseconds since the Unix epoch, `uint64`/`int64` depending on the transport.
The library uses `std::chrono::system_clock` by default; applications may pass
their own clock.

## 4. Transports

### 4.1 MCAP (`MCAPSink`)

- Writer profile: `data_tamer`. Compression: none, or `zstd` when enabled.
- One MCAP **schema** record per channel schema: `name` = `<channel_name>::<hash>`,
  `encoding` = `data_tamer`, `data` = the schema text of section 2.
- One MCAP **channel** record per channel: `topic` = the channel name,
  `messageEncoding` = `data_tamer`, `metadata` empty.
- Each **message** has `logTime` = `publishTime` = the snapshot timestamp in
  nanoseconds, `sequence` = 1, and this body:

```
uint32 mask_length        little-endian
uint8  mask[mask_length]
uint32 payload_length     little-endian
uint8  payload[payload_length]
```

  The body ends exactly after the payload. `snapshot_full.mcap_message` in the
  vectors directory is one such body.

A file from a process that was killed has no footer; recover it with
`mcap recover` from the official CLI before decoding.

### 4.2 ROS 2 (`ROS2PublisherSink`)

Two topics under a user-chosen prefix:

- `<prefix>/schemas`, type `data_tamer_msgs/msg/Schemas`, reliable and
  transient-local, republished whenever a schema is added. Each entry carries
  `uint64 hash`, `string channel_name`, `string schema_text` (section 2).
- `<prefix>/data`, type `data_tamer_msgs/msg/Snapshot`: `uint64 timestamp_nsec`,
  `uint64 schema_hash`, `uint8[] active_mask`, `uint8[] payload`.

## 5. Schema hash

`schema_hash` identifies a schema within one recording or one ROS session:
`MCAPSink` maps it to a channel, the ROS subscriber maps it to a schema text.
It is defined byte for byte so that any decoder can verify it:

    schema_hash = FNV-1a-64( schema text with the "### hash:" line removed )

with the standard FNV-1a 64-bit parameters, offset basis `0xcbf29ce484222325`
and prime `0x100000001b3`, applied to the UTF-8 bytes of the text exactly as
written by the producer (section 2), minus the first line that starts with
`### hash:` (the header line) and its terminating newline; a line inside an
opaque schema body is not removed. Everything else contributes: version, channel
name, top-level fields, custom type sections and opaque encodings. Any change
to the schema, including inside a custom type body, changes the hash (up to the
collision probability of a 64-bit hash), and the same schema hashes identically
on every platform.

Decoders match a snapshot to a schema by comparing the snapshot's hash with the
`### hash:` line of the schema shipped next to it (MCAP: the schema record
attached to the channel; ROS: the `Schemas` entry). Verification against the
recomputed value is optional (`check_hash` in the C++ parser, `verify_hash` in
the Python decoder) and only meaningful for version 5 texts.

**Version 4 texts** carried a hash computed with `std::hash`, which is
implementation-defined and did not cover custom type bodies. Readers match them
by the declared value; the C++ parser still recomputes the version 4 recipe for
`check_hash`, which only agrees on the writer's platform.
Parsers older than version 5 recompute the version-4 hash themselves and
therefore reject version 5 files with "Wrong SCHEMA_VERSION" instead of
decoding them wrongly.

## 6. Worked example

`snapshot_full.payload` from the vectors directory, for the schema in
`schema.txt` (all 16 fields enabled, mask `ff ff`):

```
01                       flag     bool     true
41                       letter   char     'A'
f8                       i8       int8     -8
c8                       u8       uint8    200
c0 f9                    i16      int16    -1600
60 ea                    u16      uint16   60000
00 1e fb ff              i32      int32    -320000
00 28 6b ee              u32      uint32   4000000000
00 c0 87 82 fe ff ff ff  i64      int64    -6400000000
08 07 06 05 04 03 02 01  u64      uint64   0x0102030405060708
00 00 c0 3f              f32      float32  1.5
00 00 00 00 00 00 02 c0  f64      float64  -2.25
03 00 00 00              vec      count    3
00 00 00 00 00 00 24 40  vec[0]   float64  10.0
00 00 00 00 00 00 34 40  vec[1]   float64  20.0
00 00 00 00 00 00 3e 40  vec[2]   float64  30.0
01 00 00 00              arr[0]   int32    1        (fixed array: no count)
fe ff ff ff              arr[1]   int32    -2
03 00 00 00              arr[2]   int32    3
fc ff ff ff              arr[3]   int32    -4
00 00 00 00 00 00 f0 3f  pose/position/x   1.0     (struct: fields in MSG order)
00 00 00 00 00 00 00 40  pose/position/y   2.0
00 00 00 00 00 00 08 40  pose/position/z   3.0
2a 00 00 00              pose/stamp        42
02 00 00 00              points   count    2
00 00 00 00 00 00 10 40  points[0]/x       4.0
...                      points[0]/y,z, points[1]/x,y,z
00 00 00 00 00 00 22 40  points[1]/z       9.0
```

`snapshot_masked.*` records the same values after disabling `i16` (bit 4) and
`pose` (bit 14): mask `ef bf`, payload 30 bytes shorter, every other byte at the
same relative position.

## 7. Conformance

A decoder conforms when it reproduces `expected.json` from the vectors
directory the way `python/test_data_tamer_parser.py` does: parse `schema.txt`
and recompute its hash,
decode both snapshots from mask and payload (disabled fields absent, not zero;
floats compared bit-exactly), split both `.mcap_message` bodies into the same
mask and payload bytes, and reject a payload with trailing bytes or a schema
with another version.
