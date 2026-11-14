#!/usr/bin/env python3
"""Tests for tools/unzip -- verifies extraction and --exclude-regex member filtering."""

import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[2] / "tools" / "unzip"

MEMBERS = [
    "pkg/CMakeLists.txt",
    "pkg/tools/codegen.jar",
    "pkg/generated/src/aws-cpp-sdk-s3/s3.cpp",
    "pkg/generated/src/aws-cpp-sdk-s3-crt/s3crt.cpp",
    "pkg/generated/src/aws-cpp-sdk-sso/sso.cpp",
    "pkg/generated/src/aws-cpp-sdk-sso-admin/admin.cpp",
]


class UnzipShimTest(unittest.TestCase):
    def _unzip(self, *args: str) -> set[str]:
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            archive = tmp / "a.zip"
            with zipfile.ZipFile(archive, "w") as zf:
                for name in MEMBERS:
                    zf.writestr(name, name)
            out = tmp / "out"
            result = subprocess.run(
                [sys.executable, str(SCRIPT), "-q", *args, str(archive), "-d", str(out)],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            return {p.relative_to(out).as_posix() for p in out.rglob("*") if p.is_file()}

    def test_extracts_every_member_by_default(self) -> None:
        self.assertEqual(self._unzip(), set(MEMBERS))

    def test_exclude_regex_skips_matching_members(self) -> None:
        got = self._unzip(
            "--exclude-regex",
            r"^[^/]+/(tools/|generated/src/aws-cpp-sdk-(?!(s3|sso)/))",
        )
        self.assertEqual(
            got,
            {
                "pkg/CMakeLists.txt",
                "pkg/generated/src/aws-cpp-sdk-s3/s3.cpp",
                "pkg/generated/src/aws-cpp-sdk-sso/sso.cpp",
            },
        )


if __name__ == "__main__":
    unittest.main()
