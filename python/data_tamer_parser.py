"""Reference decoder for the Data Tamer wire format.

Pure standard library. The format is specified in docs/wire_format.md; this
module is kept deliberately literal so it can be ported to another language
line by line. Usage:

    schema = parse_schema(schema_text)
    values = parse_snapshot(schema, active_mask, payload)   # {"pose/position/x": 1.0, ...}

For an MCAP message body written by MCAPSink use split_mcap_message() first.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field

SCHEMA_VERSION = 5
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


def _parse_field_line(line: str) -> Field:
    type_part, _, name = line.partition(" ")
    name = name.strip()
    if not name:
        raise ValueError(f"field line without a name: {line!r}")
    bracket = type_part.find("[")
    if bracket < 0:
        return Field(name, type_part)
    inner = type_part[bracket + 1:type_part.index("]", bracket)]
    return Field(name, type_part[:bracket], True, int(inner) if inner else 0)


def schema_hash(text: str) -> int:
    """FNV-1a 64 of the schema text with its '### hash:' line removed (spec section 5)."""
    lines = [l for l in text.split("\n") if not l.startswith("### hash:")]
    h = 0xCBF29CE484222325
    for byte in "\n".join(lines).encode("utf-8"):
        h = ((h ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def parse_schema(text: str, verify_hash: bool = False) -> Schema:
    """Parse the schema text stored in the MCAP schema record / Schema.msg.

    With verify_hash the declared hash must equal schema_hash(text); only version 5
    texts can be verified.
    """
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


def get_bit(mask: bytes, index: int) -> bool:
    return bool(mask[index >> 3] & (1 << (index & 7)))


class _Reader:
    def __init__(self, data: bytes):
        self.data = data
        self.pos = 0

    def number(self, type_name: str):
        fmt = _STRUCT[type_name]
        if self.pos + fmt.size > len(self.data):
            raise ValueError("payload truncated")
        (value,) = fmt.unpack_from(self.data, self.pos)
        self.pos += fmt.size
        return value.decode("latin-1") if type_name == "char" else value


def _parse_field(f: Field, schema: Schema, reader: _Reader, prefix: str, out: dict) -> None:
    name = f.field_name if not prefix else f"{prefix}/{f.field_name}"
    if f.is_vector:
        count = f.array_size or reader.number("uint32")  # dynamic vector: count prefix
        names = [f"{name}[{i}]" for i in range(count)]
    else:
        names = [name]
    if f.is_basic:
        for n in names:
            out[n] = reader.number(f.type_name)
    elif f.type_name in schema.custom_types:
        subs = schema.custom_types[f.type_name]
        for n in names:
            for sub in subs:
                _parse_field(sub, schema, reader, n, out)
    else:
        raise ValueError(f"type {f.type_name!r} has an opaque encoding; cannot continue")


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
