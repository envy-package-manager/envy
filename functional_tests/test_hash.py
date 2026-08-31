"""Functional tests for hash command."""

import base64
import hashlib
import tempfile
import unittest
from pathlib import Path

from . import test_config

# 1x1 red PNG (69 bytes)
TEST_PNG_BASE64 = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVQImQEBAP7///8AAAEABQABAAAAAElFTkSuQmCC"
TEST_PNG_SHA256 = "4ef9eb6a6a63f6cb017233d2ee2087ae5e13787801e969ef8150a2c008c5795a"


class TestHash(unittest.TestCase):
    """Tests for 'envy hash' command."""

    def setUp(self):
        self.envy = test_config.get_envy_executable()
        self.tmpdir = tempfile.mkdtemp(prefix="envy-hash-test-")

    def tearDown(self):
        import shutil

        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def test_hash_draws_a_progress_bar(self):
        """Hashing a large file is a wait; its length is known, so it is a bar."""
        import os

        test_file = Path(self.tmpdir) / "big.bin"
        test_file.write_bytes(os.urandom(8 * 1024 * 1024))

        env = test_config.get_test_env()
        env["TERM"] = "dumb"
        env["ENVY_TEST_FALLBACK_THROTTLE_MS"] = "0"
        result = test_config.run(
            [str(self.envy), "hash", str(test_file)],
            capture_output=True,
            text=True,
            env=env,
        )

        self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
        rows = [ln.strip() for ln in result.stderr.splitlines() if "[hash]" in ln]
        self.assertTrue(rows, f"hash drew no progress row: {result.stderr}")
        self.assertTrue(
            rows[-1].endswith(": 100.0%"), f"hash did not finish on a full bar: {rows}"
        )
        self.assertIn("hashing", rows[-1], f"row does not name the work: {rows[-1]}")

    def test_hash_bar_lands_above_its_digest(self):
        """The bar is the record of work already reported; it belongs above the digest,
        not under it. One merged stream, so the interleaving is observable."""
        import os
        import subprocess

        test_file = Path(self.tmpdir) / "big.bin"
        test_file.write_bytes(os.urandom(4 * 1024 * 1024))

        env = test_config.get_test_env()
        env["TERM"] = "dumb"
        env["ENVY_TEST_FALLBACK_THROTTLE_MS"] = "0"
        result = test_config.run(
            [str(self.envy), "hash", str(test_file)],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            env=env,
        )

        self.assertEqual(0, result.returncode, f"output: {result.stdout}")
        lines = result.stdout.splitlines()
        rows = [i for i, ln in enumerate(lines) if "[hash]" in ln]
        digests = [
            i for i, ln in enumerate(lines) if "big.bin" in ln and "[hash]" not in ln
        ]
        self.assertTrue(rows, f"hash drew no progress row: {lines}")
        self.assertTrue(digests, f"hash printed no digest: {lines}")
        self.assertLess(
            rows[-1],
            digests[0],
            f"finished bar landed below the digest it was drawn for: {lines}",
        )

    def test_hash_binary_file_matches_external_tool(self):
        """Verify envy hash matches external SHA256 computation (ground truth)."""
        # Write test PNG to temp file
        test_file = Path(self.tmpdir) / "test.png"
        test_file.write_bytes(base64.b64decode(TEST_PNG_BASE64))

        # Compute expected hash with Python's hashlib (ground truth)
        with open(test_file, "rb") as f:
            expected_hash = hashlib.sha256(f.read()).hexdigest()

        # Verify against hardcoded expected value
        self.assertEqual(
            expected_hash,
            TEST_PNG_SHA256,
            "Test PNG SHA256 mismatch - check base64 encoding",
        )

        # Compute hash with envy
        result = test_config.run(
            [str(self.envy), "hash", str(test_file)],
            capture_output=True,
            text=True,
        )

        self.assertEqual(result.returncode, 0, f"Hash command failed: {result.stderr}")
        # Output format: <64hex>  <filename>
        line = result.stdout.strip()
        parts = line.split("  ", 1)
        self.assertEqual(len(parts), 2, f"Expected sha256sum format, got: {line}")
        envy_hash = parts[0]

        # Verify envy hash matches Python hashlib (ground truth)
        self.assertEqual(
            envy_hash,
            expected_hash,
            f"envy hash doesn't match Python hashlib: {envy_hash} != {expected_hash}",
        )

    def test_hash_nonexistent_file_fails(self):
        """Hash command fails gracefully on nonexistent file."""
        result = test_config.run(
            [str(self.envy), "hash", "/nonexistent/file.txt"],
            capture_output=True,
            text=True,
        )

        self.assertNotEqual(result.returncode, 0, "Should fail on nonexistent file")
        self.assertIn("not exist", result.stderr.lower())

    def test_hash_directory_processes_tar_zst(self):
        """Hash command on directory iterates .tar.zst files."""
        # Create a .tar.zst file in the temp dir
        test_file = Path(self.tmpdir) / "test.tar.zst"
        test_file.write_bytes(b"test archive data")

        result = test_config.run(
            [str(self.envy), "hash", self.tmpdir],
            capture_output=True,
            text=True,
        )

        self.assertEqual(result.returncode, 0, f"Hash command failed: {result.stderr}")
        lines = [l for l in result.stdout.strip().split("\n") if l.strip()]
        self.assertEqual(len(lines), 1)
        self.assertIn("test.tar.zst", lines[0])

    def test_hash_missing_argument_fails(self):
        """Hash command fails when file argument is missing."""
        result = test_config.run(
            [str(self.envy), "hash"],
            capture_output=True,
            text=True,
        )

        self.assertNotEqual(result.returncode, 0, "Should fail when argument missing")


if __name__ == "__main__":
    unittest.main()
