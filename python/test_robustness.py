"""Unusual and malformed input for the reference decoder.

The C++ parser tests (data_tamer_cpp/tests/parser_robustness_tests.cpp) check the same
cases, so the two decoders stay consistent.
"""
import struct
import time
import unittest

import data_tamer_parser as dt

SEPARATOR = "=" * 59
HEADER = "### version: 5\n### hash: 0\n### channel_name: c\n\n"


class StringMembers(unittest.TestCase):
    def test_string_member_decodes_as_a_char_vector(self):
        # the C++ writer sends a std::string as char[]: a uint32 count, then the characters
        text = HEADER + f"Named n\n{SEPARATOR}\nMSG: Named\nuint8 tag\nchar[] name\n"
        values = dt.parse_snapshot(dt.parse_schema(text), b"\x01", bytes([1, 3, 0, 0, 0]) + b"abc")
        self.assertEqual(values, {"n/tag": 1, "n/name[0]": "a", "n/name[1]": "b", "n/name[2]": "c"})


class Booleans(unittest.TestCase):
    def test_any_non_zero_byte_is_true(self):
        schema = dt.parse_schema(HEADER + "bool flag\n")
        for byte, expected in ((0, False), (1, True), (2, True), (255, True)):
            with self.subTest(byte=byte):
                self.assertIs(dt.parse_snapshot(schema, b"\x01", bytes([byte]))["flag"], expected)


class WorkBound(unittest.TestCase):
    """A count read from the payload, or the length of a fixed array, must not make the
    decoder work more than the payload can pay for."""

    INSTANT = 0.25  # seconds; the old decoder needs seconds for the zero-size cases

    def timed_parse(self, text: str, payload: bytes):
        schema = dt.parse_schema(HEADER + text)
        start = time.perf_counter()
        values = dt.parse_snapshot(schema, b"\x01", payload)
        return values, time.perf_counter() - start

    def test_huge_count_of_zero_size_elements_costs_nothing(self):
        # E has no field, so an element takes no byte: the payload does not bound the count
        values, seconds = self.timed_parse(f"E[] e\n{SEPARATOR}\nMSG: E\n",
                                           struct.pack("<I", 0x02000000))
        self.assertEqual(values, {})
        self.assertLess(seconds, self.INSTANT)

    def test_nested_fixed_arrays_of_zero_size_elements_cost_nothing(self):
        values, seconds = self.timed_parse(
            f"L1[5000] top\n{SEPARATOR}\nMSG: E\n{SEPARATOR}\nMSG: L1\nE[5000] e\n", b"")
        self.assertEqual(values, {})
        self.assertLess(seconds, self.INSTANT)

    def test_count_larger_than_the_payload_can_hold_is_rejected(self):
        text = f"Pair[] pairs\n{SEPARATOR}\nMSG: Pair\nuint32 a\nuint32 b\n"
        with self.assertRaises(ValueError):  # 3 pairs need 24 bytes, 20 follow
            self.timed_parse(text, struct.pack("<I", 3) + bytes(20))
        values, _ = self.timed_parse(text, struct.pack("<I", 2) + bytes(16))  # an exact fit
        self.assertEqual(len(values), 4)

    def test_fixed_array_larger_than_the_payload_is_rejected(self):
        with self.assertRaises(ValueError):
            self.timed_parse("uint8[8] a\n", bytes(7))

    def test_elements_that_hold_a_vector_take_at_least_its_count(self):
        text = f"Blob[] blobs\n{SEPARATOR}\nMSG: Blob\nuint8[] bytes\n"
        values, _ = self.timed_parse(text, struct.pack("<I", 3) + bytes(12))  # 3 empty blobs
        self.assertEqual(values, {})
        with self.assertRaises(ValueError):
            self.timed_parse(text, struct.pack("<I", 3) + bytes(11))

    def test_cyclic_type_in_a_vector_is_rejected(self):
        text = f"A[] as\n{SEPARATOR}\nMSG: A\nA a\n"
        with self.assertRaises(ValueError):
            self.timed_parse(text, struct.pack("<I", 1))
        values, _ = self.timed_parse(text, struct.pack("<I", 0))  # no element, no cycle
        self.assertEqual(values, {})


class SchemaHeaders(unittest.TestCase):
    def test_empty_text_is_rejected(self):
        for text in ("", "\n", "  \n\r\n   \n"):
            with self.subTest(text=text), self.assertRaises(ValueError):
                dt.parse_schema(text)

    def test_text_with_headers_only_is_a_schema_without_fields(self):
        schema = dt.parse_schema("### version: 5\n### hash: 9\n### channel_name: c\n")
        self.assertEqual((schema.hash, schema.channel_name, schema.fields), (9, "c", []))

    def test_header_value_may_follow_the_colon_directly(self):
        schema = dt.parse_schema(
            "### version:5\n### hash:7\n### channel_name:my chan\nfloat64 x\n")
        self.assertEqual((schema.hash, schema.channel_name), (7, "my chan"))
        self.assertEqual(schema.fields, [dt.Field("x", "float64")])


if __name__ == "__main__":
    unittest.main()
