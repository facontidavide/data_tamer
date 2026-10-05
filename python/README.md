# data-tamer-parser

Reference decoder for the [Data Tamer](https://github.com/PlotJuggler/data_tamer)
wire format, specified in
[docs/wire_format.md](https://github.com/PlotJuggler/data_tamer/blob/V2/docs/wire_format.md)
(the V2 branch, until it is merged).
A single module, `data_tamer_parser`, using the Python standard library only.

```
pip install data-tamer-parser           # the decoder
pip install "data-tamer-parser[mcap]"   # plus the mcap package, for iter_mcap()
pip install ./python                    # from a checkout of the repository
```

## MCAP files written by MCAPSink

```python
import data_tamer_parser as dt

for timestamp_nsec, topic, values in dt.iter_mcap("log.mcap"):
    print(topic, timestamp_nsec, values)   # values: {"pose/position/x": 1.0, "vec[2]": 3.0, ...}
```

Disabled fields are absent from `values`. Channels with another encoding are skipped.
`iter_mcap()` reads one file: `MCAPSink` can split a recording into numbered files
(`run.mcap`, `run_1.mcap`, ...), each self-contained, so read them in turn.

## Field names from a schema

`Schema.field_names()` lists the flattened names a schema can produce, without
decoding a message, so fields disabled in a given message are listed too:

```python
schema = dt.parse_schema(schema_text)
schema.field_names()   # ["pose/position/x", "arr[0]", "arr[1]", "vec[]", "points[]/x", ...]
```

Nested fields join with `/` and fixed arrays (`T[N]`) are expanded to `[0]`..`[N-1]`.
A dynamic vector (`T[]`) has a length known only per message, so its elements are
listed once with empty brackets (`vec[]`, `points[]/x`), where the decoder produces
`vec[0]`, `vec[1]`, ... The `[]` is a placeholder: a field whose own name ends in `[]`
would look the same. A field of an opaque custom type is listed under its own name, but
`parse_snapshot()` cannot decode a snapshot where it is enabled. `ValueError` is raised
for undefined or cyclic types, nesting deeper than the decoder accepts
(`MAX_SCHEMA_DEPTH`) and schemas that expand to more than `MAX_FIELD_NAMES`
(1,000,000) names.

## Versions and releases

The package is released together with the C++ library: a release tag, the
`package.xml` versions and `data_tamer_parser.__version__` are the same `X.Y.Z`. The
`python` workflow publishes to PyPI only for tags of that form, and fails if the tag
differs from `__version__`.

## Lower level

```python
schema = dt.parse_schema(schema_text)                  # line format or YAML
mask, payload = dt.split_mcap_message(mcap_message_body)
values = dt.parse_snapshot(schema, mask, payload)
```

ROS 2 messages of `ROS2PublisherSink` (`data_tamer_msgs` `Schemas`, `Snapshot`,
`SnapshotBatch`) are decoded with `SchemaRegistry`, `parse_snapshot_msg()` and
`iter_snapshot_batch()`; they only read message attributes and need no ROS import.

```python
registry = dt.SchemaRegistry()     # registry.add_schemas(schemas_msg) for <prefix>/schemas
for schema, timestamp_nsec, values in dt.iter_snapshot_batch(registry, batch_msg):
    ...
```
