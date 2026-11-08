#!/usr/bin/env python3
"""Check that every place that carries the library version agrees.

Run from anywhere: python3 tools/check_versions.py. Exit status 0 when all
agree, 1 otherwise. The version lives in:

- data_tamer_cpp/CMakeLists.txt  project(... VERSION x.y.z): the shared library
  VERSION and SOVERSION (= major) and DATA_TAMER_VERSION derive from it;
- data_tamer_cpp/package.xml and data_tamer_msgs/package.xml  <version>;
- data_tamer_cpp/conanfile.py  version = "x.y.z";
- python/data_tamer_parser.py  __version__;
- data_tamer_foxglove/package.json  version;
- data_tamer_foxglove/extension/package.json  version;
- data_tamer_cpp/CHANGELOG.rst and data_tamer_msgs/CHANGELOG.rst  the first
  section of both is "Unreleased" (the release commit renames both: bloom makes
  each package's debian/changelog from its own) or carries the same number.
"""
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent


def first_match(relative, pattern, flags=0):
    text = (ROOT / relative).read_text(encoding="utf-8")
    match = re.search(pattern, text, flags)
    if not match:
        sys.exit(f"{relative}: pattern {pattern!r} not found")
    return match.group(1)


def main():
    versions = {
        "data_tamer_cpp/CMakeLists.txt": first_match(
            "data_tamer_cpp/CMakeLists.txt",
            r"^\s*project\(\s*data_tamer_v2\s+VERSION\s+(\d+\.\d+\.\d+)", re.M),
        "data_tamer_cpp/package.xml": first_match(
            "data_tamer_cpp/package.xml", r"<version>([^<]+)</version>"),
        "data_tamer_msgs/package.xml": first_match(
            "data_tamer_msgs/package.xml", r"<version>([^<]+)</version>"),
        "data_tamer_cpp/conanfile.py": first_match(
            "data_tamer_cpp/conanfile.py", r'^\s*version\s*=\s*"([^"]+)"', re.M),
        "python/data_tamer_parser.py": first_match(
            "python/data_tamer_parser.py", r'^__version__\s*=\s*"([^"]+)"', re.M),
    }
    for package in ("data_tamer_foxglove/package.json",
                    "data_tamer_foxglove/extension/package.json"):
        versions[package] = json.loads(
            (ROOT / package).read_text(encoding="utf-8"))["version"]
    expected = versions["data_tamer_cpp/CMakeLists.txt"]
    ok = True
    for name, version in versions.items():
        if version != expected:
            print(f"{name}: {version}, expected {expected}")
            ok = False

    heads = {
        name: first_match(name, r"^\^+\n.*\n\^+\n\n(.+)\n-+\n", re.M).split()[0]
        for name in ("data_tamer_cpp/CHANGELOG.rst", "data_tamer_msgs/CHANGELOG.rst")
    }
    if len(set(heads.values())) != 1 or set(heads.values()) - {"Unreleased", expected}:
        for name, head in heads.items():
            print(f"{name}: head section is {head!r}")
        print(f"expected 'Unreleased' or {expected}, the same in both")
        ok = False

    if ok:
        print(f"all versions are {expected}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
