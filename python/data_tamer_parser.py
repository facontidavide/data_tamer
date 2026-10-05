"""Reference decoder for the Data Tamer wire format.

Pure standard library. The format is specified in docs/wire_format.md; this
module is kept deliberately literal so it can be ported to another language
line by line. Usage:

    schema = parse_schema(schema_text)
    values = parse_snapshot(schema, active_mask, payload)   # {"pose/position/x": 1.0, ...}

For an MCAP message body written by MCAPSink use split_mcap_message() first.
parse_schema() reads both schema renderings: the line format (version 5) and
YAML (version 6); to_text() renders a schema back to the line format.
For the ROS 2 messages of ROS2PublisherSink (data_tamer_msgs Schemas, Snapshot,
SnapshotBatch) use SchemaRegistry, parse_snapshot_msg() and
iter_snapshot_batch(); they only read message attributes, so they need no ROS
import:

    registry = SchemaRegistry()
    registry.add_schemas(schemas_msg)            # optional if the batch embeds them
    for schema, timestamp_nsec, values in iter_snapshot_batch(registry, batch_msg):
        ...

Schema.field_names() lists the flattened names from the schema alone, and
iter_mcap(path) reads a whole MCAP file (needs the optional `mcap` package):

    for timestamp_nsec, topic, values in iter_mcap("log.mcap"):
        ...
"""
from __future__ import annotations

__version__ = "2.0.0"

import struct
from dataclasses import dataclass, field

SCHEMA_VERSION = 5
SCHEMA_YAML_VERSION = 6  # the YAML rendering of the same schema (spec section 2.1)
_READABLE_VERSIONS = (4, 5)  # 4 differs only in how its hash was computed

# Basic type name -> little-endian struct; the order is the BasicType id order.
_STRUCT = {
    "bool": struct.Struct("<?"), "char": struct.Struct("<c"),
    "int8": struct.Struct("<b"), "uint8": struct.Struct("<B"),
    "int16": struct.Struct("<h"), "uint16": struct.Struct("<H"),
    "int32": struct.Struct("<i"), "uint32": struct.Struct("<I"),
    "int64": struct.Struct("<q"), "uint64": struct.Struct("<Q"),
    "float32": struct.Struct("<f"), "float64": struct.Struct("<d"),
}


@dataclass
class Field:
    """Mirrors TypeField in the C++ headers."""
    field_name: str
    type_name: str           # one of _STRUCT, or a custom type name
    is_vector: bool = False  # true for "T[]" and "T[N]"
    array_size: int = 0      # N for "T[N]", 0 for "T[]" (count prefix on the wire)

    @property
    def is_basic(self) -> bool:
        return self.type_name in _STRUCT


@dataclass
class Schema:
    channel_name: str = ""
    hash: int = 0
    fields: list[Field] = field(default_factory=list)
    custom_types: dict[str, list[Field]] = field(default_factory=dict)
    # opaque custom encodings: type name -> (encoding, schema text)
    custom_schemas: dict[str, tuple[str, str]] = field(default_factory=dict)

    def field_names(self) -> list[str]:
        """The flattened names parse_snapshot() can produce, in payload order,
        computed from the schema alone (no message, so disabled fields are listed too).

        Names are built as in the decoder: "pose/position/x", "arr[3]". The
        length of a dynamic vector ("T[]") is only known per message, so its
        elements are listed once with empty brackets: "vec[]", "points[]/x"
        (the decoder emits "vec[0]", "points[1]/x", ...). "[]" is a placeholder:
        a field whose own name ends in "[]" would be listed the same way. A field of an opaque
        custom type is listed under its own name; its content is not described
        by the schema, and parse_snapshot() raises ValueError when it is enabled.

        Raises ValueError for an undefined or cyclic custom type, for nesting
        deeper than parse_snapshot() accepts (MAX_SCHEMA_DEPTH), and when the
        schema would expand to more than MAX_FIELD_NAMES names.
        """
        counts: dict[str, tuple[int, int]] = {}
        total = sum(_names_count(f, self, counts, 0) for f in self.fields)
        if total > MAX_FIELD_NAMES:
            raise ValueError(f"schema expands to {total} names, more than {MAX_FIELD_NAMES}")
        out: list[str] = []
        for f in self.fields:
            _field_names(f, self, "", out)
        return out


def _parse_type_spec(spec: str, name: str) -> Field:
    """`T`, `T[]` or `T[N]` with N in 1..65535 (spec section 2)."""
    bracket = spec.find("[")
    type_name = spec if bracket < 0 else spec[:bracket]
    if not type_name:
        raise ValueError(f"empty type in {spec!r}")
    if bracket < 0:
        return Field(name, type_name)
    inner = spec[bracket + 1:-1]
    if not spec.endswith("]") or not all(c in "0123456789" for c in inner):
        raise ValueError(f"invalid type {spec!r}")
    if inner and not 1 <= int(inner) <= 65535:
        raise ValueError(f"array size out of range (1..65535) in {spec!r}")
    return Field(name, type_name, True, int(inner) if inner else 0)


def _parse_field_line(line: str) -> Field:
    type_part, _, name = line.partition(" ")
    name = name.strip()
    if not name:
        raise ValueError(f"field line without a name: {line!r}")
    return _parse_type_spec(type_part, name)


def _parse_uint(value: str, what: str) -> int:
    if not value or len(value) > 20 or not all(c in "0123456789" for c in value):
        raise ValueError(f"invalid {what} {value!r}")
    number = int(value)
    if number > 0xFFFFFFFFFFFFFFFF:  # as the C++ reader: it must fit in a uint64
        raise ValueError(f"invalid {what} {value!r}")
    return number


def schema_hash(text: str) -> int:
    """FNV-1a 64 of the schema text with its '### hash:' line removed (spec section 5)."""
    lines = text.split("\n")
    for i, l in enumerate(lines):  # exactly the header line, as the C++ writer does
        if l.startswith("### hash:"):
            del lines[i]
            break
    h = 0xCBF29CE484222325
    for byte in "\n".join(lines).encode("utf-8"):
        h = ((h ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def parse_schema(text: str, verify_hash: bool = False) -> Schema:
    """Parse the schema text stored in the MCAP schema record / Schema.msg.

    Both renderings are accepted: the line format (versions 4 and 5) and YAML
    (version 6), told apart by their first line. With verify_hash the declared
    hash must equal the computed one (section 5); version 4 cannot be verified.
    """
    if _first_line(text).startswith("version:"):
        return _parse_schema_yaml(text, verify_hash)
    schema = Schema()
    lines = iter(text.splitlines())
    target = schema.fields
    last_type = ""
    for raw in lines:
        line = raw.strip()
        if not line or line.startswith("====="):
            continue
        if line.startswith("### version:"):
            if int(line.split(":", 1)[1]) not in _READABLE_VERSIONS:
                raise ValueError(f"unsupported schema version in {line!r}")
        elif line.startswith("### hash:"):
            schema.hash = int(line.split(":", 1)[1])
        elif line.startswith("### channel_name:"):
            schema.channel_name = line.split(":", 1)[1].strip()
        elif line.startswith("MSG: "):
            last_type = line[5:].strip()
            target = schema.custom_types.setdefault(last_type, [])
        elif line.startswith("ENCODING: "):
            # Opaque sections come last and own the rest of the text (spec section 2).
            schema.custom_schemas[last_type] = (line[10:].strip(), "\n".join(lines))
            del schema.custom_types[last_type]
            break
        else:
            target.append(_parse_field_line(line))
    if verify_hash and schema.hash != schema_hash(text):
        raise ValueError("schema hash does not match its text")
    return schema


def _first_line(text: str) -> str:
    for raw in text.split("\n"):
        line = raw.strip(" \r")
        if line and not line.startswith("#"):
            return line
    return ""


def _field_line(f: Field) -> str:
    if not f.is_vector:
        return f"{f.type_name} {f.field_name}"
    return f"{f.type_name}[{f.array_size or ''}] {f.field_name}"


def to_text(schema: Schema) -> str:
    """Render a schema in the version 5 line format, byte for byte as the C++
    writer does; schema_hash() of the result is the schema hash (section 5)."""
    sep = "=" * 59 + "\n"
    out = [f"### version: {SCHEMA_VERSION}\n### hash: {schema.hash}\n"
           f"### channel_name: {schema.channel_name}\n\n"]
    out += [_field_line(f) + "\n" for f in schema.fields]
    for name in sorted(schema.custom_types):
        out.append(f"{sep}MSG: {name}\n")
        out += [_field_line(f) + "\n" for f in schema.custom_types[name]]
    for name in sorted(schema.custom_schemas):
        encoding, body = schema.custom_schemas[name]
        out.append(f"{sep}MSG: {name}\nENCODING: {encoding}\n{body}\n")
    return "".join(out)


# ---- YAML rendering (spec section 2.1) ----------------------------------------
# Only the subset the writer emits is accepted: block mappings indented with
# spaces, plain or double-quoted scalars, "{}" for an empty mapping and
# full-line comments.

_ESCAPES = {'"': '"', "\\": "\\", "/": "/", "n": "\n", "t": "\t", "r": "\r", "0": "\0"}
_HEX_ESCAPES = {"x": 2, "u": 4, "U": 8}


def _read_quoted(s: str, line: str) -> tuple[str, str]:
    """Decode the double-quoted scalar at the start of s; return (value, rest)."""
    out, i = [], 1
    while i < len(s):
        c = s[i]
        if c == '"':
            return "".join(out), s[i + 1:]
        if c == "\\":
            e = s[i + 1:i + 2]
            if e in _ESCAPES:
                out.append(_ESCAPES[e])
                i += 2
            elif e in _HEX_ESCAPES:
                n = _HEX_ESCAPES[e]
                digits = s[i + 2:i + 2 + n]
                if len(digits) != n or not all(c in "0123456789abcdefABCDEF" for c in digits):
                    raise ValueError(f"bad escape in {line!r}")
                cp = int(digits, 16)
                if cp > 0x10FFFF or 0xD800 <= cp <= 0xDFFF:
                    raise ValueError(f"bad escape in {line!r}")
                out.append(chr(cp))
                i += 2 + n
            else:
                raise ValueError(f"bad escape in {line!r}")
        else:
            out.append(c)
            i += 1
    raise ValueError(f"unterminated string in {line!r}")


def _scalar(s: str, line: str) -> str:
    if s.startswith('"'):
        value, rest = _read_quoted(s, line)
        if rest.strip():
            raise ValueError(f"unexpected text after string in {line!r}")
        return value
    if s.startswith(("'", "[", "{", "&", "*", "!", "|", ">", "-", "?")) or " #" in s:
        raise ValueError(f"unsupported YAML in {line!r}")
    return s


def _split_entry(content: str, line: str) -> tuple[str, str]:
    """Split "key: value" / "key:" into (key, value text)."""
    if content.startswith('"'):
        key, rest = _read_quoted(content, line)
        if not rest.startswith(":"):
            raise ValueError(f"expected ':' after key in {line!r}")
        rest = rest[1:]
    else:
        colon = content.find(": ")
        if colon < 0:
            if not content.endswith(":"):
                raise ValueError(f"expected 'key: value' in {line!r}")
            colon = len(content) - 1
        key, rest = _scalar(content[:colon], line), content[colon + 1:]
    if rest and not rest.startswith(" "):
        raise ValueError(f"expected a space after ':' in {line!r}")
    return key, rest.strip(" ")  # only spaces, as the C++ reader


@dataclass
class _YamlLevel:
    parent_indent: int
    indent: int | None  # indentation of the mapping's entries; None if not seen yet
    entries: list
    keys: set = field(default_factory=set)


# A node is (key, scalar, children); children is None for a scalar entry.
def _parse_yaml_tree(text: str) -> list:
    root: list = []
    stack = [_YamlLevel(-1, 0, root)]  # top level starts at column 0
    for raw in text.split("\n"):
        line = raw.rstrip(" \r")
        content = line.lstrip(" ")
        if not content or content.startswith("#"):
            continue
        if content.startswith("\t"):
            raise ValueError(f"tab indentation in {line!r}")
        indent = len(line) - len(content)
        while indent <= stack[-1].parent_indent:
            stack.pop()
        level = stack[-1]
        if level.indent is None:
            level.indent = indent
        elif indent != level.indent:
            raise ValueError(f"bad indentation in {line!r}")
        key, rest = _split_entry(content, line)
        if key in level.keys:
            raise ValueError(f"duplicate key {key!r}")
        level.keys.add(key)
        if rest == "":
            if len(stack) > MAX_SCHEMA_DEPTH:
                raise ValueError(f"nesting too deep in {line!r}")
            children: list = []
            level.entries.append((key, None, children))
            stack.append(_YamlLevel(indent, None, children))
        elif rest == "{}":
            level.entries.append((key, None, []))
        else:
            level.entries.append((key, _scalar(rest, line), None))
    return root


def _flatten(nodes: list, prefix: str, out: list[Field]) -> None:
    for key, value, children in nodes:
        if children is None:
            out.append(_parse_type_spec(value, prefix + key))
        else:
            _flatten(children, prefix + key + "/", out)


def _mapping(nodes: list) -> dict:
    return {key: (value, children) for key, value, children in nodes}


def _parse_schema_yaml(text: str, verify_hash: bool) -> Schema:
    top = _mapping(_parse_yaml_tree(text))

    def scalar(key: str) -> str:
        if key not in top or top[key][0] is None:
            raise ValueError(f"YAML schema: missing {key!r}")
        return top[key][0]

    def mapping(key: str, required: bool) -> list:
        if key not in top:
            if required:
                raise ValueError(f"YAML schema: missing {key!r}")
            return []
        if top[key][1] is None:
            raise ValueError(f"YAML schema: {key!r} must be a mapping")
        return top[key][1]

    version = scalar("version")
    if _parse_uint(version, "version") != SCHEMA_YAML_VERSION:
        raise ValueError(f"unsupported YAML schema version {version!r}")
    schema = Schema(channel_name=scalar("channel_name"),
                    hash=_parse_uint(scalar("hash"), "hash"))
    _flatten(mapping("fields", True), "", schema.fields)
    for type_name, value, children in mapping("types", False):
        if children is None:
            raise ValueError(f"YAML schema: type {type_name!r} must be a mapping")
        _flatten(children, "", schema.custom_types.setdefault(type_name, []))
    for type_name, value, children in mapping("opaque_types", False):
        entry = _mapping(children or [])
        encoding, body = entry.get("encoding", (None,))[0], entry.get("schema", (None,))[0]
        if encoding is None or body is None:
            raise ValueError(f"YAML schema: opaque type {type_name!r} needs encoding and schema")
        schema.custom_schemas[type_name] = (encoding, body)
    if verify_hash and schema.hash != schema_hash(to_text(schema)):
        raise ValueError("schema hash does not match its text")
    return schema


MAX_SCHEMA_DEPTH = 64  # deeper nesting is treated as a malformed (cyclic) schema


def get_bit(mask: bytes, index: int) -> bool:
    if (index >> 3) >= len(mask):
        raise ValueError("active mask shorter than the schema")
    return bool(mask[index >> 3] & (1 << (index & 7)))


class _Reader:
    def __init__(self, data: bytes):
        self.data = data
        self.pos = 0

    def remaining(self) -> int:
        return len(self.data) - self.pos

    def number(self, type_name: str):
        fmt = _STRUCT[type_name]
        if self.pos + fmt.size > len(self.data):
            raise ValueError("payload truncated")
        (value,) = fmt.unpack_from(self.data, self.pos)
        self.pos += fmt.size
        return value.decode("latin-1") if type_name == "char" else value


def _parse_field(f: Field, schema: Schema, reader: _Reader, prefix: str, out: dict,
                 depth: int = 0) -> None:
    if depth > MAX_SCHEMA_DEPTH:
        raise ValueError("custom types nested too deeply (cyclic schema?)")
    name = f.field_name if not prefix else f"{prefix}/{f.field_name}"
    if f.is_vector:
        count = f.array_size or reader.number("uint32")  # dynamic vector: count prefix
        if f.is_basic and count * _STRUCT[f.type_name].size > reader.remaining():
            raise ValueError("payload truncated")
        names = (f"{name}[{i}]" for i in range(count))  # lazy: the count is untrusted
    else:
        names = (name,)
    if f.is_basic:
        for n in names:
            out[n] = reader.number(f.type_name)
    elif f.type_name in schema.custom_types:
        subs = schema.custom_types[f.type_name]
        for n in names:
            for sub in subs:
                _parse_field(sub, schema, reader, n, out, depth + 1)
    else:
        raise ValueError(f"type {f.type_name!r} has an opaque encoding; cannot continue")


MAX_FIELD_NAMES = 1_000_000  # Schema.field_names() refuses to list more names


def _names_count(f: Field, schema: Schema, memo: dict, depth: int) -> int:
    """How many names `f` expands to, checking its types like _parse_field() does.

    memo maps a custom type to (names per element, nesting height), or to None
    while that type is being expanded (finding it then means a cycle). Work is
    linear in the number of type references, whatever the array sizes.
    """
    if depth > MAX_SCHEMA_DEPTH:
        raise ValueError("custom types nested too deeply (cyclic schema?)")
    per_element, height = 1, 0  # basic or opaque type: one name, no nesting
    if not f.is_basic and f.type_name not in schema.custom_schemas:
        if f.type_name not in schema.custom_types:
            raise ValueError(f"type {f.type_name!r} is not defined in the schema")
        if f.type_name in memo:
            if memo[f.type_name] is None:
                raise ValueError(f"custom type {f.type_name!r} contains itself (cyclic schema)")
            per_element, height = memo[f.type_name]
            if depth + height > MAX_SCHEMA_DEPTH:
                raise ValueError("custom types nested too deeply")
        else:
            memo[f.type_name] = None
            subs = schema.custom_types[f.type_name]
            per_element = sum(_names_count(sub, schema, memo, depth + 1) for sub in subs)
            height = 1 + max((memo[sub.type_name][1] for sub in subs
                              if memo.get(sub.type_name)), default=0) if subs else 0
            memo[f.type_name] = (per_element, height)
    return per_element * (f.array_size or 1)


def _field_names(f: Field, schema: Schema, prefix: str, out: list[str]) -> None:
    """The names _parse_field() would produce for `f`; "[]" for a dynamic vector
    element. The schema was checked by _names_count() first."""
    name = f.field_name if not prefix else f"{prefix}/{f.field_name}"
    if not f.is_vector:
        names = [name]
    elif f.array_size:
        names = [f"{name}[{i}]" for i in range(f.array_size)]
    else:
        names = [f"{name}[]"]
    subs = schema.custom_types.get(f.type_name) if not f.is_basic else None
    if f.type_name in schema.custom_schemas:
        subs = None
    for n in names:
        if subs is None:  # basic type, or opaque: not expandable from the schema
            out.append(n)
        else:
            for sub in subs:
                _field_names(sub, schema, n, out)


def parse_snapshot(schema: Schema, active_mask: bytes, payload: bytes) -> dict[str, object]:
    """Decode one snapshot into {"field/path[i]": value}. Disabled fields are absent."""
    out: dict[str, object] = {}
    reader = _Reader(payload)
    for index, f in enumerate(schema.fields):
        if get_bit(active_mask, index):
            _parse_field(f, schema, reader, "", out)
    if reader.pos != len(payload):
        raise ValueError(f"{len(payload) - reader.pos} trailing bytes in payload")
    return out


def split_mcap_message(data: bytes) -> tuple[bytes, bytes]:
    """Split an MCAPSink message body into (active_mask, payload)."""
    (mask_len,) = struct.unpack_from("<I", data, 0)
    mask = data[4:4 + mask_len]
    (payload_len,) = struct.unpack_from("<I", data, 4 + mask_len)
    start = 8 + mask_len
    payload = data[start:start + payload_len]
    if start + payload_len != len(data):
        raise ValueError("MCAP message body has trailing bytes")
    return mask, payload


class SchemaRegistry:
    """Schemas by hash, filled from the `schemas` topic and/or embedded schemas."""

    def __init__(self, verify_hash: bool = False):
        self.verify_hash = verify_hash
        self._schemas: dict[int, Schema] = {}

    def add(self, hash_value: int, schema_text: str) -> Schema:
        """Parse and store `schema_text` under `hash_value`, unless already known."""
        schema = self._schemas.get(hash_value)
        if schema is None:
            schema = parse_schema(schema_text, verify_hash=self.verify_hash)
            if schema.hash != hash_value:
                # snapshots carry hash_value: this schema would never match them
                raise ValueError(f"schema text declares hash {schema.hash}, expected {hash_value}")
            self._schemas[hash_value] = schema
        return schema

    def add_schemas(self, msg) -> None:
        """Add every entry of `msg.schemas`: a Schemas or SnapshotBatch message."""
        for schema_msg in msg.schemas:
            self.add(schema_msg.hash, schema_msg.schema_text)

    def find(self, hash_value: int) -> Schema | None:
        return self._schemas.get(hash_value)

    def __len__(self) -> int:
        return len(self._schemas)


def parse_snapshot_msg(schema: Schema, msg) -> dict[str, object]:
    """Decode a data_tamer_msgs Snapshot (needs `active_mask` and `payload`)."""
    return parse_snapshot(schema, bytes(msg.active_mask), bytes(msg.payload))


def iter_snapshot_batch(registry: SchemaRegistry, batch):
    """Iterate over (schema, timestamp_nsec, values) for each snapshot of a SnapshotBatch.

    The schemas embedded in the batch are added to `registry` right away, when
    this is called (a malformed one raises ValueError); snapshots whose schema
    is still unknown are skipped.
    """
    registry.add_schemas(batch)
    return ((schema, msg.timestamp_nsec, parse_snapshot_msg(schema, msg))
            for msg in batch.snapshots
            if (schema := registry.find(msg.schema_hash)) is not None)


MCAP_ENCODING = "data_tamer"  # schema encoding and message encoding in MCAP (spec section 4.1)


def iter_mcap(source, topics=None, verify_hash: bool = False):
    """Iterate over (timestamp_nsec, topic, values) for each Data Tamer message of an MCAP file.

    `source` is a path or a seekable binary file object; `topics` optionally
    restricts the channels read. Messages come in log time order (the snapshot
    timestamps); channels with another encoding are skipped. Needs the `mcap`
    package (pip install "data-tamer-parser[mcap]"), imported by this call.

    One call reads one file. MCAPSink can split a recording into numbered
    files (run.mcap, run_1.mcap, ...); each is self-contained, so read them
    one after the other.
    """
    try:
        from mcap.reader import make_reader
    except ImportError as err:
        raise ImportError('iter_mcap() needs the "mcap" package: '
                          'pip install "data-tamer-parser[mcap]"') from err
    return _iter_mcap(make_reader, source, topics, verify_hash)


def _iter_mcap(make_reader, source, topics, verify_hash: bool):
    if isinstance(source, (str, bytes)) or hasattr(source, "__fspath__"):
        with open(source, "rb") as stream:
            yield from _iter_mcap(make_reader, stream, topics, verify_hash)
        return
    schemas: dict[int, Schema] = {}  # by MCAP schema id
    for mcap_schema, channel, message in make_reader(source).iter_messages(topics=topics):
        if (mcap_schema is None or mcap_schema.encoding != MCAP_ENCODING
                or channel.message_encoding != MCAP_ENCODING):
            continue
        schema = schemas.get(mcap_schema.id)
        if schema is None:
            schema = parse_schema(mcap_schema.data.decode("utf-8"), verify_hash=verify_hash)
            schemas[mcap_schema.id] = schema
        mask, payload = split_mcap_message(message.data)
        yield message.log_time, channel.topic, parse_snapshot(schema, mask, payload)
