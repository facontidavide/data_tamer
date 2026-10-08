"""Unusual and malformed input for the reference decoder.

The C++ parser tests (data_tamer_cpp/tests/parser_robustness_tests.cpp) check the same
cases, so the two decoders stay consistent.
"""
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


if __name__ == "__main__":
    unittest.main()
