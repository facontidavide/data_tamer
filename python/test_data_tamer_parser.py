"""Decodes the golden vectors from docs/wire_format/vectors with the reference decoder."""
import array
import json
import pathlib
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


if __name__ == "__main__":
    unittest.main()
