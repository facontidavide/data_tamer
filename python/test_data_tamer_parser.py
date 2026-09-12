"""Decodes the golden vectors from docs/wire_format/vectors with the reference decoder."""
import json
import pathlib
import unittest

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


if __name__ == "__main__":
    unittest.main()
