"""Functional tests for engine declarative fetch per-file caching.

Tests package cache management: per-file caching across partial failures,
corruption detection/recovery, and SHA256-based revalidation.

Partial-failure states are produced by pointing one FETCH entry at a file that
does not exist yet: the other entries download and land in fetch/, that one
fails, and the run aborts. Creating the file between runs completes the spec.
The spec text is the cache key, so both runs must use the identical entry list
-- the missing entry has to stay in the spec rather than being added later.
"""

import os
import shutil
import tempfile
from pathlib import Path
import unittest

from . import test_config
from .env import EnvyTestCase
from .trace_parser import TraceParser

# Inline test files for fetch caching tests
TEST_FILES = {
    "simple.lua": "-- Simple test script for lua_util tests\nexpected_value = 42\n",
    "print_single.lua": 'print("hello")\n',
    "print_multiple.lua": 'print("a", "b", "c")\n',
}


class TestEngineFetchCaching(EnvyTestCase):
    """Tests for per-file package caching and recovery."""

    def setUp(self):
        super().setUp()
        self.test_files_dir = self.make_temp_dir("test_files_dir")
        # Enable trace for all tests if ENVY_TEST_TRACE is set
        self.trace_flag = ["--trace"] if os.environ.get("ENVY_TEST_TRACE") else []

        # Write inline test files to temp directory
        for name, content in TEST_FILES.items():
            (self.test_files_dir / name).write_text(content, encoding="utf-8")

    def lua_path(self, filename: str) -> str:
        """Get Lua-safe path to test file."""
        return (self.test_files_dir / filename).as_posix()

    def get_file_hash(self, filepath):
        """Get SHA256 hash of file using envy hash command."""
        result = test_config.run(
            [str(self.envy), "hash", str(filepath)],
            capture_output=True,
            text=True,
            check=True,
        )
        return result.stdout.strip().split("  ", 1)[0]

    def empty_file_hash(self, scratch_dir):
        """SHA256 of an empty file -- what a to-be-created missing file will hash to."""
        probe = scratch_dir / "empty_probe.txt"
        probe.write_text("")
        return self.get_file_hash(probe)

    def write_fetch_spec(self, dest_dir, sources):
        """Write the shared fetch spec; `sources` is a list of (source, sha256|None)."""
        entries = "".join(
            f'  {{ source = "{src}"'
            + (f', sha256 = "{sha}"' if sha else "")
            + " },\n"
            for src, sha in sources
        )
        path = dest_dir / "fetch_array.lua"
        path.write_text(
            f'IDENTITY = "local.fetch_array@v1"\n\nFETCH = {{\n{entries}}}\n',
            encoding="utf-8",
        )
        return path

    def run_engine(self, cache_root, spec, trace_file=None):
        """Install the spec through a generated manifest, with verbose logging."""
        manifest = test_config.write_spec_manifest(
            Path(spec).parent, [("local.fetch_array@v1", spec)]
        )
        return test_config.run(
            [
                str(self.envy),
                f"--cache-root={cache_root}",
                f"--trace=file:{trace_file}" if trace_file else "--trace",
                "--verbose",
                "install",
                "--manifest",
                str(manifest),
            ],
            capture_output=True,
            text=True,
        )

    def sole_variant_dir(self, cache_root):
        """The single cache variant directory for local.fetch_array@v1."""
        identity_dir = cache_root / "packages" / "local.fetch_array@v1"
        self.assertTrue(
            identity_dir.exists(), f"Identity dir should exist: {identity_dir}"
        )
        variant_dirs = list(identity_dir.glob("*-blake3-*"))
        self.assertEqual(
            len(variant_dirs), 1, f"Expected 1 variant dir, found: {variant_dirs}"
        )
        return variant_dirs[0]

    def partial_fetch_spec(self, cache_root):
        """Spec whose last entry is missing; returns (spec_path, missing_file).

        Run it once to get a failed run with the other files cached in fetch/,
        then create `missing_file` to let a second run complete.
        """
        missing_file = cache_root / "temp_files" / "fetch_missing.lua"
        missing_file.parent.mkdir(parents=True, exist_ok=True)
        spec = self.write_fetch_spec(
            cache_root,
            [
                (
                    self.lua_path("simple.lua"),
                    self.get_file_hash(self.test_files_dir / "simple.lua"),
                ),
                (
                    self.lua_path("print_single.lua"),
                    self.get_file_hash(self.test_files_dir / "print_single.lua"),
                ),
                # No sha256 - should still work (permissive mode)
                (self.lua_path("print_multiple.lua"), None),
                (
                    f"file://{missing_file.as_posix()}",
                    self.empty_file_hash(cache_root),
                ),
            ],
        )
        return spec, missing_file

    def test_declarative_fetch_partial_failure_then_complete(self):
        """Partial failure caches successful files; completion reuses them."""
        # Use shared cache root so second run sees first run's cached files
        shared_cache = self.make_temp_dir("shared_cache")

        try:
            spec, missing_file = self.partial_fetch_spec(shared_cache)

            # Run 1: three files succeed, the missing one fails the run
            result1 = self.run_engine(shared_cache, spec)
            self.assertNotEqual(result1.returncode, 0, "Expected partial failure")
            self.assertIn(
                "fetch failed",
                result1.stderr.lower(),
                f"Expected fetch failure: {result1.stderr}",
            )

            # Successful downloads survive the failed run in the package cache
            fetch_dir = self.sole_variant_dir(shared_cache) / "fetch"
            self.assertTrue(
                (fetch_dir / "simple.lua").exists(), "simple.lua should be cached"
            )
            self.assertTrue(
                (fetch_dir / "print_single.lua").exists(),
                "print_single.lua should be cached",
            )

            # Empty file matches the SHA256 the spec declared for it
            missing_file.write_text("")

            # Run 2: completion with cache - same cache root
            trace_file2 = shared_cache / "trace2.jsonl"
            result2 = self.run_engine(shared_cache, spec, trace_file2)
            self.assertEqual(
                result2.returncode, 0, f"Second run failed: {result2.stderr}"
            )

            # Structured trace: the two sha-bearing files are reused from the
            # per-file cache; the previously-missing file and the un-hashed one
            # are downloaded.
            parser2 = TraceParser(trace_file2)
            skipped = parser2.filter_by_spec_and_event(
                "local.fetch_array@v1", "download_skipped"
            )
            completed = parser2.filter_by_spec_and_event(
                "local.fetch_array@v1", "download_complete"
            )
            self.assertEqual(
                len(skipped), 2, f"Expected 2 cached files reused, got: {skipped}"
            )
            self.assertEqual(
                len(completed), 2, f"Expected 2 files downloaded, got: {completed}"
            )

            # Same facts on the log sink, which tests the verbose narrative too
            self.assertIn(
                "cached (sha ok)",
                result2.stderr.lower(),
                f"Expected per-file cache reuse log: {result2.stderr}",
            )
            self.assertIn(
                "downloading 2 file(s)",
                result2.stderr,
                f"Expected 2 downloads in second run: {result2.stderr}",
            )
        finally:
            shutil.rmtree(shared_cache, ignore_errors=True)

    def test_declarative_fetch_corrupted_cache(self):
        """Corrupted files in fetch/ are detected and re-downloaded."""
        shared_cache = self.make_temp_dir("shared_cache")

        try:
            spec, missing_file = self.partial_fetch_spec(shared_cache)

            # Run 1: populates fetch/ but fails on the missing entry
            self.assertNotEqual(self.run_engine(shared_cache, spec).returncode, 0)

            variant_dir = self.sole_variant_dir(shared_cache)
            fetch_dir = variant_dir / "fetch"

            # Corrupt simple.lua (garbage that won't match its SHA256)
            (fetch_dir / "simple.lua").write_text(
                "GARBAGE CONTENT THAT WILL FAIL SHA256 VERIFICATION"
            )
            missing_file.write_text("")

            # Run 2: should detect corruption and re-download
            result = self.run_engine(shared_cache, spec)
            self.assertEqual(result.returncode, 0, f"Should succeed: {result.stderr}")
            self.assertIn(
                "sha mismatch",
                result.stderr.lower(),
                f"Expected sha mismatch detection: {result.stderr}",
            )

            # Package completed: fetch/ is deleted, entry marker and pkg/ remain
            self.assertTrue(
                (variant_dir / "envy-complete").exists(),
                "Entry-level completion marker should exist after successful install",
            )
            self.assertTrue(
                (variant_dir / "pkg").exists(),
                "Package directory should exist after completion",
            )
        finally:
            shutil.rmtree(shared_cache, ignore_errors=True)

    def test_declarative_fetch_complete_but_unmarked(self):
        """All files present with correct SHA256, but no completion marker."""
        shared_cache = self.make_temp_dir("shared_cache")

        try:
            spec, missing_file = self.partial_fetch_spec(shared_cache)

            # Run 1: establishes the cache structure, then fails
            self.assertNotEqual(self.run_engine(shared_cache, spec).returncode, 0)

            variant_dir = self.sole_variant_dir(shared_cache)
            fetch_dir = variant_dir / "fetch"
            fetch_dir.mkdir(parents=True, exist_ok=True)

            # Hand-populate every fetch entry so the content is complete but
            # unmarked. The fourth entry's source is deliberately left absent, so
            # the only way the run below can succeed is by reusing this cached
            # copy -- a reuse regression fails outright instead of re-downloading.
            for name in TEST_FILES:
                shutil.copy(self.test_files_dir / name, fetch_dir / name)
            (fetch_dir / missing_file.name).write_text("")

            completion_marker = fetch_dir / "envy-complete"
            completion_marker.unlink(missing_ok=True)

            # Run 2: should verify cached files by SHA256 and reuse them
            result = self.run_engine(shared_cache, spec)
            self.assertEqual(result.returncode, 0, f"Should succeed: {result.stderr}")

            # All three sha-bearing entries are reused on content alone
            self.assertEqual(
                result.stderr.lower().count("cached (sha ok)"),
                3,
                f"Expected all hashed files reused: {result.stderr}",
            )
            # print_multiple.lua has no SHA256 so it cannot be trusted, and it is
            # the only one -- this count is what holds the premise above honest.
            self.assertIn(
                "downloading 1 file(s)",
                result.stderr,
                f"Only the un-hashed file should download: {result.stderr}",
            )

            # fetch/ and its marker are deleted after successful completion
            self.assertTrue(
                (variant_dir / "envy-complete").exists(),
                "Entry-level completion marker should exist after successful install",
            )
        finally:
            shutil.rmtree(shared_cache, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
