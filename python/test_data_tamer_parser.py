"""Decodes the golden vectors from docs/wire_format/vectors with the reference decoder."""
import array
import io
import json
import pathlib
import re
import unittest
from types import SimpleNamespace

import data_tamer_parser as dt

VECTORS = pathlib.Path(__file__).resolve().parent.parent / "docs" / "wire_format" / "vectors"


def read(name: str) -> bytes:
    return (VECTORS / name).read_bytes()


class GoldenVectors(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.schema = dt.parse_schema(read("schema.txt").decode(), verify_hash=True)
        cls.expected = json.loads(read("expected.json"))

    def test_schema(self):
        self.assertEqual(self.schema.channel_name, "wire_test")
        self.assertEqual(self.schema.hash, dt.schema_hash(read("schema.txt").decode()))
        self.assertEqual([f.field_name for f in self.schema.fields], self.expected["fields"])
        self.assertEqual(sorted(self.schema.custom_types), ["Point3D", "Pose"])

    def decode(self, stem: str) -> dict:
        mask, payload = read(stem + ".mask"), read(stem + ".payload")
        self.assertEqual(dt.split_mcap_message(read(stem + ".mcap_message")), (mask, payload))
        return dt.parse_snapshot(self.schema, mask, payload)

    def test_full_snapshot(self):
        self.assertEqual(self.decode("snapshot_full"), self.expected["full"])

    def test_masked_snapshot(self):
        values = self.decode("snapshot_masked")
        self.assertEqual(values, self.expected["masked"])
        self.assertNotIn("i16", values)
        self.assertNotIn("pose/stamp", values)

    def test_rejects_trailing_bytes_and_wrong_version(self):
        with self.assertRaises(ValueError):
            dt.parse_snapshot(self.schema, read("snapshot_full.mask"), read("snapshot_full.payload") + b"\0")
        with self.assertRaises(ValueError):
            dt.parse_schema("### version: 3\n")
        with self.assertRaises(ValueError):
            dt.parse_schema("### version: 5\n### hash: 1\n### channel_name: x\n", verify_hash=True)


class FieldNames(unittest.TestCase):
    """Schema.field_names() lists the decoder's names without decoding a message."""

    def test_golden_schema(self):
        schema = dt.parse_schema(read("schema.txt").decode())
        names = schema.field_names()
        self.assertEqual(names, [
            "flag", "letter", "i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64",
            "f32", "f64", "vec[]", "arr[0]", "arr[1]", "arr[2]", "arr[3]",
            "pose/position/x", "pose/position/y", "pose/position/z", "pose/stamp",
            "points[]/x", "points[]/y", "points[]/z"])
        self.assertEqual(dt.parse_schema(read("schema.yaml").decode()).field_names(), names)
        # the decoded names, in payload order, once dynamic indices become "[]"
        for key in ("full", "masked"):
            decoded = json.loads(read("expected.json"))[key]
            templates = dict.fromkeys(re.sub(r"^(vec|points)\[\d+\]", r"\1[]", n) for n in decoded)
            self.assertEqual(list(templates), [n for n in names if n in templates])
        self.assertNotIn("i16", json.loads(read("expected.json"))["masked"])  # listed anyway

    def test_nested_schema(self):
        schema = dt.parse_schema(read("schema_nested.yaml").decode())
        names = schema.field_names()
        self.assertEqual(names[:-4], [f.field_name for f in schema.fields[:-1]])  # basic types
        self.assertEqual(names[-4:], ["cart/pose/position/x", "cart/pose/position/y",
                                      "cart/pose/position/z", "cart/pose/stamp"])
        self.assertEqual(dt.parse_schema(read("schema_nested.txt").decode()).field_names(), names)

    def test_nested_containers_and_opaque_types(self):
        text = ("### version: 5\n### hash: 0\n### channel_name: c\n\n"
                "Leg[2] legs\nBlob blob\nBlob[] blobs\n"
                "=====\nMSG: Leg\nfloat32[] q\nint8[2] mode\n"
                "=====\nMSG: Blob\nENCODING: proto\nmessage Blob {}\n")
        self.assertEqual(dt.parse_schema(text).field_names(), [
            "legs[0]/q[]", "legs[0]/mode[0]", "legs[0]/mode[1]",
            "legs[1]/q[]", "legs[1]/mode[0]", "legs[1]/mode[1]", "blob", "blobs[]"])

    def test_rejects_undefined_and_cyclic_types(self):
        with self.assertRaises(ValueError):
            dt.parse_schema("### version: 5\n### hash: 0\n### channel_name: c\n\nFoo x\n").field_names()
        cyclic = "### version: 5\n### hash: 0\n### channel_name: c\n\nA a\n=====\nMSG: A\nA a\n"
        for text in (cyclic, cyclic.replace("A a\n", "A[60000] a\n")):  # no blowup
            with self.subTest(text=text), self.assertRaises(ValueError):
                dt.parse_schema(text).field_names()

    @staticmethod
    def chain(depth: int, leaf: str = "float32 x") -> str:
        """Top field of type T1, T1 holds a T2, ..., T<depth> holds `leaf`."""
        types = "".join(f"=====\nMSG: T{i}\nT{i + 1} t\n" for i in range(1, depth))
        return (f"### version: 5\n### hash: 0\n### channel_name: c\n\nT1 t\n{types}"
                f"=====\nMSG: T{depth}\n{leaf}\n")

    def test_depth_limit_matches_the_decoder(self):
        ok = dt.parse_schema(self.chain(dt.MAX_SCHEMA_DEPTH))
        self.assertEqual(ok.field_names(), ["t/" * dt.MAX_SCHEMA_DEPTH + "x"])
        self.assertEqual(dt.parse_snapshot(ok, b"\x01", b"\0" * 4), {"t/" * 64 + "x": 0.0})
        for depth in (dt.MAX_SCHEMA_DEPTH + 1, 80, 1200):  # 1200: no RecursionError
            schema = dt.parse_schema(self.chain(depth))
            with self.subTest(depth=depth), self.assertRaises(ValueError):
                schema.field_names()
            with self.subTest(depth=depth), self.assertRaises(ValueError):
                dt.parse_snapshot(schema, b"\x01", b"\0" * 4)
        # a deep type reached again, deeper, through the memo
        text = ("### version: 5\n### hash: 0\n### channel_name: c\n\nT1 a\nW w\n"
                "=====\nMSG: W\nT1 b\n" + self.chain(dt.MAX_SCHEMA_DEPTH).split("T1 t\n", 1)[1])
        with self.assertRaises(ValueError):
            dt.parse_schema(text).field_names()

    def test_size_limit(self):
        text = ("### version: 5\n### hash: 0\n### channel_name: c\n\nA[65535] a\n"
                "=====\nMSG: A\nB[65535] b\n=====\nMSG: B\nfloat32[65535] x\n")
        with self.assertRaisesRegex(ValueError, "more than"):
            dt.parse_schema(text).field_names()  # 65535**3 names: refused, not listed
        wide = ("### version: 5\n### hash: 0\n### channel_name: c\n\nA[1000] a\n"
                "=====\nMSG: A\nfloat32[1000] x\n")
        self.assertEqual(len(dt.parse_schema(wide).field_names()), dt.MAX_FIELD_NAMES)


class YamlSchema(unittest.TestCase):
    """The YAML rendering (version 6) decodes to the same schema as the line format."""

    def assert_same(self, stem: str):
        text = read(stem + ".txt").decode()
        yaml_text = read(stem + ".yaml").decode()
        from_text = dt.parse_schema(text, verify_hash=True)
        from_yaml = dt.parse_schema(yaml_text, verify_hash=True)
        self.assertEqual(from_yaml, from_text)
        self.assertEqual(dt.to_text(from_yaml), text)
        return from_yaml

    def test_golden_schema(self):
        schema = self.assert_same("schema")
        mask, payload = read("snapshot_full.mask"), read("snapshot_full.payload")
        self.assertEqual(dt.parse_snapshot(schema, mask, payload),
                         json.loads(read("expected.json"))["full"])

    def test_nested_schema(self):
        schema = self.assert_same("schema_nested")
        self.assertEqual(schema.channel_name, "nested test")
        names = [f.field_name for f in schema.fields]
        self.assertEqual(names[:5], ["arm/joint_1/position", "arm/joint_1/velocity",
                                     "arm/joint_2/position", "arm/joint_2/velocity", "arm/mode"])
        self.assertIn("on", names)
        self.assertIn("7up/y", names)
        self.assertLess(len(read("schema_nested.yaml")), len(read("schema_nested.txt")))

    def test_quoted_scalars_and_opaque_types(self):
        text = ('version: 6\nhash: 0\nchannel_name: "a \\"b\\""\nfields:\n'
                '  "x y":\n    "1": "Blob[2]"\n'
                'opaque_types:\n  Blob:\n    encoding: proto\n    schema: "l1\\n\\tl2\\x01\\u00e9"\n')
        schema = dt.parse_schema(text)
        self.assertEqual(schema.channel_name, 'a "b"')
        self.assertEqual(schema.fields, [dt.Field("x y/1", "Blob", True, 2)])
        self.assertEqual(schema.custom_schemas, {"Blob": ("proto", "l1\n\tl2\x01\u00e9")})

    def test_rejects_malformed_yaml(self):
        head = "version: 6\nhash: 1\nchannel_name: c\n"
        for bad in ["version: 7\nhash: 1\nchannel_name: c\nfields: {}\n",
                    head,                                            # no fields
                    head + "fields:\n  a: float64\n  a: int8\n",     # duplicate key
                    head + "fields:\n  a: float64\n   b: int8\n",    # indentation
                    head + "fields:\n  a: [float64]\n",              # flow sequence
                    head + 'fields:\n  "a: float64\n',               # unterminated
                    head + "fields:\n  a: int32[0]\n",               # extent out of range
                    head + "fields:\n  a: int32[70000]\n",
                    head + "fields:\n  a: int32[-1]\n",
                    head + "fields:\n  a: int32[1_0]\n",
                    head + 'fields:\n  a: "[3]"\n',                  # empty type
                    head + 'fields:\n  "\\x+1": int8\n',            # bad hex escape
                    head + 'fields:\n  "\\uD800": int8\n',          # surrogate
                    "version: +6\nhash: 1\nchannel_name: c\nfields: {}\n",
                    "version: 6\nhash: 18446744073709551616\nchannel_name: c\nfields: {}\n",
                    "  version: 6\n  hash: 1\n  channel_name: c\n  fields: {}\n",
                    head + "fields:\n" + "".join("  " * (i + 1) + "k:\n" for i in range(100))
                    + "  " * 101 + "x: int8\n"]:                      # too deep
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                dt.parse_schema(bad)
        with self.assertRaises(ValueError):
            dt.parse_schema(head + "fields:\n  a: float64\n", verify_hash=True)

    def test_generic_yaml_loader_agrees(self):
        try:
            import yaml
        except ImportError:
            self.skipTest("PyYAML not installed")
        doc = yaml.safe_load(read("schema_nested.yaml"))
        self.assertEqual(doc["version"], dt.SCHEMA_YAML_VERSION)
        self.assertEqual(list(doc["fields"]["arm"]), ["joint_1", "joint_2", "mode"])
        self.assertEqual(doc["fields"]["7up"], {"x": "float64", "y": "float64"})
        self.assertEqual(doc["fields"]["on"], "uint8")


class RosMessageHelpers(unittest.TestCase):
    """SnapshotBatch decoding, with stand-ins that have the data_tamer_msgs attributes."""

    def setUp(self):
        self.text = read("schema.txt").decode()
        self.schema_hash = dt.parse_schema(self.text).hash
        self.expected = json.loads(read("expected.json"))

    def snapshot_msg(self, stem: str, timestamp: int, schema_hash=None):
        # rclpy maps uint8[] to array('B')
        return SimpleNamespace(
            timestamp_nsec=timestamp,
            schema_hash=self.schema_hash if schema_hash is None else schema_hash,
            active_mask=array.array("B", read(stem + ".mask")),
            payload=array.array("B", read(stem + ".payload")))

    def batch(self, embed: bool):
        schemas = [SimpleNamespace(hash=self.schema_hash, channel_name="wire_test",
                                   schema_text=self.text)] if embed else []
        return SimpleNamespace(schemas=schemas, snapshots=[
            self.snapshot_msg("snapshot_full", 10),
            self.snapshot_msg("snapshot_masked", 20),
            self.snapshot_msg("snapshot_full", 30, schema_hash=self.schema_hash + 1),
        ])

    def test_self_contained_batch(self):
        registry = dt.SchemaRegistry(verify_hash=True)
        decoded = list(dt.iter_snapshot_batch(registry, self.batch(embed=True)))
        self.assertEqual(len(registry), 1)
        self.assertEqual([t for _, t, _ in decoded], [10, 20])  # unknown hash skipped
        self.assertEqual(decoded[0][0].channel_name, "wire_test")
        self.assertEqual(decoded[0][2], self.expected["full"])
        self.assertEqual(decoded[1][2], self.expected["masked"])

    def test_batch_with_schemas_from_topic(self):
        registry = dt.SchemaRegistry()
        self.assertEqual(list(dt.iter_snapshot_batch(registry, self.batch(embed=False))), [])
        registry.add_schemas(SimpleNamespace(schemas=self.batch(embed=True).schemas))
        self.assertEqual(len(list(dt.iter_snapshot_batch(registry, self.batch(embed=False)))), 2)

    def test_registry_rejects_hash_mismatch(self):
        with self.assertRaises(ValueError):
            dt.SchemaRegistry().add(self.schema_hash + 1, self.text)

    def test_single_snapshot_msg(self):
        schema = dt.parse_schema(self.text)
        self.assertEqual(dt.parse_snapshot_msg(schema, self.snapshot_msg("snapshot_full", 0)),
                         self.expected["full"])


def write_mcap(stream, channels):
    """An MCAP file as MCAPSink writes it (spec section 4.1): channels are
    (topic, schema text, [(timestamp, message body)][, schema encoding]), plus a
    non Data Tamer channel."""
    from mcap.writer import Writer
    writer = Writer(stream)
    writer.start(profile="data_tamer", library="test")
    other_schema = writer.register_schema(name="Other", encoding="jsonschema", data=b"{}")
    other = writer.register_channel(topic="other", message_encoding="json", schema_id=other_schema)
    writer.add_message(other, log_time=5, publish_time=5, data=b"{}", sequence=1)
    for topic, text, messages, *encoding in channels:
        schema = dt.parse_schema(text)
        schema_id = writer.register_schema(name=f"{schema.channel_name}::{schema.hash}",
                                           encoding=(encoding or ["data_tamer"])[0],
                                           data=text.encode())
        channel_id = writer.register_channel(topic=topic, message_encoding="data_tamer",
                                             schema_id=schema_id)
        for timestamp, body in messages:
            writer.add_message(channel_id, log_time=timestamp, publish_time=timestamp,
                               data=body, sequence=1)
    writer.finish()


class Mcap(unittest.TestCase):
    """iter_mcap() over files built from the golden vectors with the `mcap` package."""

    def setUp(self):
        try:
            import mcap  # noqa: F401
        except ImportError:
            self.skipTest("mcap not installed (pip install mcap)")
        self.expected = json.loads(read("expected.json"))
        full, masked = read("snapshot_full.mcap_message"), read("snapshot_masked.mcap_message")
        self.stream = io.BytesIO()
        write_mcap(self.stream, [
            ("wire_test", read("schema.txt").decode(), [(30, full), (10, masked)]),
            ("as_yaml", read("schema.yaml").decode(), [(20, full)]),
            ("wrong_schema_encoding", read("schema.txt").decode(), [(15, full)], "ros2msg"),
        ])

    def test_iter_mcap(self):
        self.stream.seek(0)
        decoded = list(dt.iter_mcap(self.stream, verify_hash=True))
        self.assertEqual([(t, topic) for t, topic, _ in decoded],
                         [(10, "wire_test"), (20, "as_yaml"), (30, "wire_test")])
        self.assertEqual([v for _, _, v in decoded],
                         [self.expected["masked"], self.expected["full"], self.expected["full"]])

    def test_path_and_topics(self):
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "log.mcap"
            path.write_bytes(self.stream.getvalue())
            self.assertEqual(len(list(dt.iter_mcap(path))), 3)
            self.assertEqual([t for t, _, _ in dt.iter_mcap(str(path), topics=["as_yaml"])], [20])

    def test_verify_hash_rejects_wrong_hash(self):
        text = read("schema.txt").decode()
        declared = dt.parse_schema(text).hash
        stream = io.BytesIO()
        write_mcap(stream, [("tampered", text.replace(f"hash: {declared}", f"hash: {declared + 1}"),
                             [(1, read("snapshot_full.mcap_message"))])])
        stream.seek(0)
        self.assertEqual(len(list(dt.iter_mcap(stream))), 1)  # not checked by default
        stream.seek(0)
        with self.assertRaises(ValueError):
            list(dt.iter_mcap(stream, verify_hash=True))


if __name__ == "__main__":
    unittest.main()
