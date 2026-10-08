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


class SchemaNumbers(unittest.TestCase):
    TAIL = "### channel_name: c\n\nfloat64 x\n"

    def test_version_and_hash_must_be_unsigned_decimals(self):
        for bad in ("", "x", "-1", "+5", "12abc", "5.0", "1e3", "0x10", "1_0", "\u0665",
                    "99999999999999999999999"):  # "\u0665" is an Arabic-Indic digit
            with self.subTest(bad=bad, header="hash"), self.assertRaises(ValueError):
                dt.parse_schema(f"### version: 5\n### hash: {bad}\n" + self.TAIL)
            with self.subTest(bad=bad, header="version"), self.assertRaises(ValueError):
                dt.parse_schema(f"### version: {bad}\n### hash: 1\n" + self.TAIL)
        largest = dt.parse_schema("### version: 5\n### hash: 18446744073709551615\n" + self.TAIL)
        self.assertEqual(largest.hash, 2**64 - 1)


class CustomTypeSections(unittest.TestCase):
    def test_separator_is_a_line_of_at_least_30_equal_signs(self):
        for length in (30, 59, 100):
            with self.subTest(length=length):
                text = HEADER + "Pose p\n" + "=" * length + " \r\nMSG: Pose\nfloat64 x\n"
                self.assertEqual(dt.parse_schema(text).custom_types,
                                 {"Pose": [dt.Field("x", "float64")]})

    def test_shorter_run_of_equal_signs_is_not_a_separator(self):
        with self.assertRaises(ValueError):
            dt.parse_schema(HEADER + "Pose p\n" + "=" * 29 + "\nMSG: Pose\nfloat64 x\n")

    def test_field_name_holding_equal_signs_is_not_a_separator(self):
        name = "a" + "=" * 35 + "b"
        schema = dt.parse_schema(HEADER + f"float64 {name}\nint8 after\n")
        self.assertEqual([f.field_name for f in schema.fields], [name, "after"])
        self.assertEqual(schema.custom_types, {})

    def test_type_name_line_is_trimmed_and_may_follow_blank_lines(self):
        for msg_line in ("  MSG: Pose", "MSG: Pose  \r", "\n \n  MSG:   Pose"):
            with self.subTest(msg_line=msg_line):
                text = HEADER + f"Pose p\n{SEPARATOR}\n{msg_line}\nfloat64 x\n"
                self.assertEqual(list(dt.parse_schema(text).custom_types), ["Pose"])

    def test_separator_not_followed_by_a_type_name_is_rejected(self):
        for after in ("float64 x\n", "MSG:\n", ""):
            with self.subTest(after=after), self.assertRaises(ValueError):
                dt.parse_schema(HEADER + f"Pose p\n{SEPARATOR}\n{after}")


class OpaqueSections(unittest.TestCase):
    """An `ENCODING:` section is an opaque custom type (docs/wire_format.md)."""

    def test_encoding_section_is_an_opaque_custom_type(self):
        text = (HEADER + "Foreign f\nPose p\n"
                + f"{SEPARATOR}\nMSG: Pose\nfloat64 x\n"
                + f"{SEPARATOR}\nMSG: Foreign\nENCODING: ros2msg\nfloat64 a\nstring b\n")
        schema = dt.parse_schema(text)
        self.assertEqual(schema.custom_schemas, {"Foreign": ("ros2msg", "float64 a\nstring b")})
        self.assertEqual(list(schema.custom_types), ["Pose"])
        self.assertEqual(dt.to_text(schema), text)

    def test_opaque_section_owns_the_rest_of_the_text(self):
        # the foreign schema has sections of its own: none of them is ours
        foreign = "float64 a\n" + SEPARATOR + "\nMSG: Inner\nint32 b\n\n"
        schema = dt.parse_schema(HEADER + "Foreign f\n" + SEPARATOR
                                 + "\nMSG: Foreign\nENCODING: proto \r\n" + foreign)
        self.assertEqual(schema.custom_schemas, {"Foreign": ("proto", foreign[:-1])})
        self.assertEqual(schema.custom_types, {})

    def test_opaque_body_is_the_rest_of_the_text_without_one_final_newline(self):
        # the writer adds the newline after the body, so a body can end in a newline of its own
        for tail, body in (("ENCODING: proto", ""), ("ENCODING: proto\n", ""),
                           ("ENCODING: proto\n\n", ""), ("ENCODING: proto\n\n\n", "\n"),
                           ("ENCODING: proto\nbody", "body"), ("ENCODING: proto\nbody\n", "body"),
                           ("ENCODING: proto\nbody\n\n", "body\n")):
            with self.subTest(tail=tail):
                schema = dt.parse_schema(HEADER + "Foreign f\n" + SEPARATOR
                                         + "\nMSG: Foreign\n" + tail)
                self.assertEqual(schema.custom_schemas, {"Foreign": ("proto", body)})

    def test_active_field_of_an_opaque_type_says_it_cannot_be_decoded(self):
        schema = dt.parse_schema(HEADER + "Foreign f\nForeign[] fs\n" + SEPARATOR
                                 + "\nMSG: Foreign\nENCODING: proto\nmessage Foreign {}\n")
        self.assertEqual(dt.parse_snapshot(schema, b"\x00", b""), {})
        # the field, and a non-empty vector of the type: neither can be decoded
        for mask, payload in ((b"\x01", bytes(8)), (b"\x02", bytes([1, 0, 0, 0, 0, 0, 0, 0]))):
            with self.subTest(mask=mask), self.assertRaisesRegex(ValueError, "opaque encoding"):
                dt.parse_snapshot(schema, mask, payload)

    def test_encoding_line_that_does_not_open_a_section_is_rejected(self):
        for text in (HEADER + "ENCODING: proto\n",
                     HEADER + "Pose p\n" + SEPARATOR
                     + "\nMSG: Pose\nfloat64 x\nENCODING: proto\nbody\n"):
            with self.subTest(text=text), self.assertRaises(ValueError):
                dt.parse_schema(text)


class LegacyTypeNames(unittest.TestCase):
    def test_field_named_like_a_legacy_type_keeps_its_type(self):
        # the Python decoder has no legacy mode: this is how the C++ parser reads a text
        # that has a version line
        text = (HEADER + "float64 BOOL\nuint32 INT8\nPose OTHER\nint8[2] DOUBLE\n"
                + f"{SEPARATOR}\nMSG: Pose\nfloat64 x\n")
        fields = dt.parse_schema(text).fields
        self.assertEqual([(f.type_name, f.field_name) for f in fields], [
            ("float64", "BOOL"), ("uint32", "INT8"), ("Pose", "OTHER"), ("int8", "DOUBLE")])


class McapMessageBody(unittest.TestCase):
    """uint32 mask length, mask, uint32 payload length, payload (docs/wire_format.md 4.1)."""

    def test_body_is_split_into_mask_and_payload(self):
        body = bytes([2, 0, 0, 0, 0xAB, 0xCD, 3, 0, 0, 0, 1, 2, 3])
        self.assertEqual(dt.split_mcap_message(body), (b"\xab\xcd", b"\x01\x02\x03"))
        self.assertEqual(dt.split_mcap_message(bytes(8)), (b"", b""))

    def test_malformed_body_is_a_value_error(self):
        for name, body in (("empty", b""),
                           ("mask length cut", bytes([1, 0, 0])),
                           ("mask longer than the body", bytes([9, 0, 0, 0, 1, 2])),
                           ("no payload length", bytes([2, 0, 0, 0, 1, 2])),
                           ("payload length cut", bytes([1, 0, 0, 0, 7, 1, 0])),
                           ("payload longer than the body",
                            bytes([1, 0, 0, 0, 7, 100, 0, 0, 0, 1, 2, 3])),
                           ("payload length 2^32-1",
                            bytes([1, 0, 0, 0, 7, 255, 255, 255, 255, 1, 2, 3])),
                           ("bytes after the payload",
                            bytes([1, 0, 0, 0, 7, 1, 0, 0, 0, 9, 0xEE]))):
            with self.subTest(name), self.assertRaises(ValueError):
                dt.split_mcap_message(body)


if __name__ == "__main__":
    unittest.main()
