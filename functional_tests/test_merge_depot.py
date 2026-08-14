"""Functional tests for 'envy merge-depot' command."""

import shutil
import tempfile
import threading
import unittest
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from . import test_config
from .env import EnvyTestCase

# Canonical 64-char hex hashes for test use
HASH_A = "a" * 64
HASH_B = "b" * 64
HASH_C = "c" * 64
HASH_D = "d" * 64


class _QuietHTTPHandler(SimpleHTTPRequestHandler):
    def __init__(self, *args, directory=None, **kwargs):
        super().__init__(*args, directory=directory, **kwargs)

    def log_message(self, format, *args):
        return


def make_manifest_line(sha256, path):
    return f"{sha256}  {path}\n"


class TestMergeDepot(EnvyTestCase):
    """Tests for 'envy merge-depot' command."""

    def setUp(self):
        self.test_dir = self.make_temp_dir("test_dir")
        self.envy = test_config.get_envy_executable()
        self.project_root = Path(__file__).parent.parent

    def tearDown(self):
        shutil.rmtree(self.test_dir, ignore_errors=True)

    def _write_manifest(self, name, lines):
        """Write a depot manifest file and return its path."""
        path = self.test_dir / name
        path.write_text("".join(lines), encoding="utf-8")
        return path

    def _run_merge(self, *args):
        """Run envy merge-depot and return CompletedProcess."""
        cmd = [str(self.envy), "merge-depot", *[str(a) for a in args]]
        return test_config.run(
            cmd,
            cwd=self.project_root,
            capture_output=True,
            text=True,
        )

    @staticmethod
    def _parse_output_line(line):
        """Parse a single '<hash>  <path>' line -> (hash, path_str)."""
        parts = line.strip().split("  ", 1)
        if len(parts) != 2:
            raise ValueError(f"Expected '<hash>  <path>', got: {line}")
        return parts[0], parts[1]

    def _parse_output(self, stdout):
        """Parse merge-depot stdout into list of (hash, path_str) tuples."""
        lines = [l for l in stdout.strip().split("\n") if l.strip()]
        return [self._parse_output_line(l) for l in lines]

    # --- Basic merge ---

    def test_single_manifest(self):
        """Merging a single manifest passes it through."""
        m = self._write_manifest("darwin.txt", [
            make_manifest_line(HASH_A, "pkg-darwin.tar.zst"),
        ])
        result = self._run_merge(m)
        self.assertEqual(result.returncode, 0, result.stderr)
        entries = self._parse_output(result.stdout)
        self.assertEqual(len(entries), 1)
        self.assertEqual(entries[0][0], HASH_A)

    def test_merge_two_manifests(self):
        """Merging two manifests produces union of entries."""
        darwin = self._write_manifest("darwin.txt", [
            make_manifest_line(HASH_A, "pkg-darwin.tar.zst"),
        ])
        linux = self._write_manifest("linux.txt", [
            make_manifest_line(HASH_B, "pkg-linux.tar.zst"),
        ])
        result = self._run_merge(darwin, linux)
        self.assertEqual(result.returncode, 0, result.stderr)
        entries = self._parse_output(result.stdout)
        self.assertEqual(len(entries), 2)

    def test_output_sorted_by_path(self):
        """Output lines are sorted alphabetically by path."""
        m = self._write_manifest("unsorted.txt", [
            make_manifest_line(HASH_C, "zzz.tar.zst"),
            make_manifest_line(HASH_A, "aaa.tar.zst"),
            make_manifest_line(HASH_B, "mmm.tar.zst"),
        ])
        result = self._run_merge(m)
        self.assertEqual(result.returncode, 0, result.stderr)
        entries = self._parse_output(result.stdout)
        paths = [str(e[1]) for e in entries]
        self.assertEqual(paths, ["aaa.tar.zst", "mmm.tar.zst", "zzz.tar.zst"])

    def test_deduplicates_identical_entries(self):
        """Identical entries across manifests are deduplicated."""
        a = self._write_manifest("a.txt", [
            make_manifest_line(HASH_A, "same.tar.zst"),
        ])
        b = self._write_manifest("b.txt", [
            make_manifest_line(HASH_A, "same.tar.zst"),
        ])
        result = self._run_merge(a, b)
        self.assertEqual(result.returncode, 0, result.stderr)
        entries = self._parse_output(result.stdout)
        self.assertEqual(len(entries), 1)

    # --- --existing flag ---

    def test_existing_entries_preserved(self):
        """Existing entries not in new manifests are preserved."""
        existing = self._write_manifest("existing.txt", [
            make_manifest_line(HASH_C, "old-pkg.tar.zst"),
        ])
        new = self._write_manifest("new.txt", [
            make_manifest_line(HASH_A, "new-pkg.tar.zst"),
        ])
        result = self._run_merge(new, "--existing", existing)
        self.assertEqual(result.returncode, 0, result.stderr)
        entries = self._parse_output(result.stdout)
        self.assertEqual(len(entries), 2)
        paths = {str(e[1]) for e in entries}
        self.assertIn("old-pkg.tar.zst", paths)
        self.assertIn("new-pkg.tar.zst", paths)

    def test_existing_and_new_same_hash(self):
        """Entry in both existing and new with same hash is deduplicated."""
        existing = self._write_manifest("existing.txt", [
            make_manifest_line(HASH_A, "shared.tar.zst"),
        ])
        new = self._write_manifest("new.txt", [
            make_manifest_line(HASH_A, "shared.tar.zst"),
        ])
        result = self._run_merge(new, "--existing", existing)
        self.assertEqual(result.returncode, 0, result.stderr)
        entries = self._parse_output(result.stdout)
        self.assertEqual(len(entries), 1)

    # --- Hash conflicts ---

    def test_hash_change_vs_existing_warns(self):
        """Different hash vs existing emits warning, new hash wins."""
        existing = self._write_manifest("existing.txt", [
            make_manifest_line(HASH_A, "changed.tar.zst"),
        ])
        new = self._write_manifest("new.txt", [
            make_manifest_line(HASH_B, "changed.tar.zst"),
        ])
        result = self._run_merge(new, "--existing", existing)
        self.assertEqual(result.returncode, 0, result.stderr)
        entries = self._parse_output(result.stdout)
        self.assertEqual(len(entries), 1)
        # New hash should win
        self.assertEqual(entries[0][0], HASH_B)
        # Warning should appear on stderr
        self.assertIn("hash changed", result.stderr)

    def test_hash_change_vs_existing_strict_errors(self):
        """With --strict, different hash vs existing is a hard error."""
        existing = self._write_manifest("existing.txt", [
            make_manifest_line(HASH_A, "changed.tar.zst"),
        ])
        new = self._write_manifest("new.txt", [
            make_manifest_line(HASH_B, "changed.tar.zst"),
        ])
        result = self._run_merge(new, "--existing", existing, "--strict")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("hash changed", result.stderr)

    def test_cross_input_conflict_errors(self):
        """Same path with different hashes across new inputs is always an error."""
        a = self._write_manifest("a.txt", [
            make_manifest_line(HASH_A, "conflict.tar.zst"),
        ])
        b = self._write_manifest("b.txt", [
            make_manifest_line(HASH_B, "conflict.tar.zst"),
        ])
        result = self._run_merge(a, b)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("conflicting hashes", result.stderr)

    def test_cross_input_conflict_with_existing(self):
        """Cross-input conflict detected even when path exists in --existing."""
        existing = self._write_manifest("existing.txt", [
            make_manifest_line(HASH_A, "pkg.tar.zst"),
        ])
        new1 = self._write_manifest("new1.txt", [
            make_manifest_line(HASH_B, "pkg.tar.zst"),
        ])
        new2 = self._write_manifest("new2.txt", [
            make_manifest_line(HASH_C, "pkg.tar.zst"),
        ])
        result = self._run_merge(new1, new2, "--existing", existing)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("conflicting hashes", result.stderr)

    # --- Edge cases ---

    def test_no_arguments_fails(self):
        """merge-depot with no arguments fails."""
        result = self._run_merge()
        self.assertNotEqual(result.returncode, 0)

    def test_comments_and_blanks_skipped(self):
        """Comment and blank lines in manifests are ignored."""
        m = self._write_manifest("with_comments.txt", [
            "# header comment\n",
            "\n",
            make_manifest_line(HASH_A, "pkg.tar.zst"),
            "# trailing comment\n",
        ])
        result = self._run_merge(m)
        self.assertEqual(result.returncode, 0, result.stderr)
        entries = self._parse_output(result.stdout)
        self.assertEqual(len(entries), 1)

    def test_url_prefixed_paths(self):
        """Paths with URL prefixes (from --depot-prefix) are preserved."""
        m = self._write_manifest("urls.txt", [
            make_manifest_line(HASH_A, "https://cdn.example.com/pkg.tar.zst"),
        ])
        result = self._run_merge(m)
        self.assertEqual(result.returncode, 0, result.stderr)
        entries = self._parse_output(result.stdout)
        self.assertEqual(entries[0][1], "https://cdn.example.com/pkg.tar.zst")

    def test_large_merge_superset(self):
        """Merge preserves all entries from existing + adds new ones."""
        existing_lines = [
            make_manifest_line(f"{i:064x}", f"existing-{i}.tar.zst")
            for i in range(10)
        ]
        new_lines = [
            make_manifest_line(f"{i:064x}", f"new-{i}.tar.zst")
            for i in range(100, 105)
        ]
        existing = self._write_manifest("existing.txt", existing_lines)
        new = self._write_manifest("new.txt", new_lines)
        result = self._run_merge(new, "--existing", existing)
        self.assertEqual(result.returncode, 0, result.stderr)
        entries = self._parse_output(result.stdout)
        self.assertEqual(len(entries), 15)

    # --- Remote --existing ---

    def test_existing_local_file_path(self):
        """--existing with a local file path works identically to before."""
        existing = self._write_manifest("existing.txt", [
            make_manifest_line(HASH_A, "old.tar.zst"),
        ])
        new = self._write_manifest("new.txt", [
            make_manifest_line(HASH_B, "new.tar.zst"),
        ])
        result = self._run_merge(new, "--existing", existing)
        self.assertEqual(result.returncode, 0, result.stderr)
        entries = self._parse_output(result.stdout)
        self.assertEqual(len(entries), 2)
        paths = {e[1] for e in entries}
        self.assertIn("old.tar.zst", paths)
        self.assertIn("new.tar.zst", paths)

    def test_existing_nonexistent_local_file_errors(self):
        """--existing with a nonexistent local path errors at runtime."""
        new = self._write_manifest("new.txt", [
            make_manifest_line(HASH_A, "pkg.tar.zst"),
        ])
        result = self._run_merge(new, "--existing", "/nonexistent/file.txt")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not found", result.stderr)

    def test_existing_http_url(self):
        """--existing fetches manifest from HTTP URL."""
        serve_dir = self.test_dir / "serve"
        serve_dir.mkdir()
        existing_content = (
            make_manifest_line(HASH_C, "old-remote.tar.zst")
            + make_manifest_line(HASH_D, "preserved.tar.zst")
        )
        (serve_dir / "existing.txt").write_text(existing_content, encoding="utf-8")

        handler = partial(_QuietHTTPHandler, directory=str(serve_dir))
        server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
        server_thread = threading.Thread(target=server.serve_forever, daemon=True)
        server_thread.start()
        try:
            port = server.server_address[1]
            url = f"http://127.0.0.1:{port}/existing.txt"

            new = self._write_manifest("new.txt", [
                make_manifest_line(HASH_A, "new-pkg.tar.zst"),
            ])
            result = self._run_merge(new, "--existing", url)
            self.assertEqual(result.returncode, 0, result.stderr)
            entries = self._parse_output(result.stdout)
            self.assertEqual(len(entries), 3)
            paths = {e[1] for e in entries}
            self.assertIn("old-remote.tar.zst", paths)
            self.assertIn("preserved.tar.zst", paths)
            self.assertIn("new-pkg.tar.zst", paths)
        finally:
            server.shutdown()
            server_thread.join(timeout=5)
            server.server_close()

    def test_existing_http_hash_change_warns(self):
        """Hash change vs HTTP --existing emits warning, new hash wins."""
        serve_dir = self.test_dir / "serve"
        serve_dir.mkdir()
        (serve_dir / "existing.txt").write_text(
            make_manifest_line(HASH_A, "changed.tar.zst"), encoding="utf-8"
        )

        handler = partial(_QuietHTTPHandler, directory=str(serve_dir))
        server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
        server_thread = threading.Thread(target=server.serve_forever, daemon=True)
        server_thread.start()
        try:
            port = server.server_address[1]
            url = f"http://127.0.0.1:{port}/existing.txt"

            new = self._write_manifest("new.txt", [
                make_manifest_line(HASH_B, "changed.tar.zst"),
            ])
            result = self._run_merge(new, "--existing", url)
            self.assertEqual(result.returncode, 0, result.stderr)
            entries = self._parse_output(result.stdout)
            self.assertEqual(len(entries), 1)
            self.assertEqual(entries[0][0], HASH_B)
            self.assertIn("hash changed", result.stderr)
        finally:
            server.shutdown()
            server_thread.join(timeout=5)
            server.server_close()

    def test_existing_http_strict_errors(self):
        """--strict with HTTP --existing errors on hash change."""
        serve_dir = self.test_dir / "serve"
        serve_dir.mkdir()
        (serve_dir / "existing.txt").write_text(
            make_manifest_line(HASH_A, "changed.tar.zst"), encoding="utf-8"
        )

        handler = partial(_QuietHTTPHandler, directory=str(serve_dir))
        server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
        server_thread = threading.Thread(target=server.serve_forever, daemon=True)
        server_thread.start()
        try:
            port = server.server_address[1]
            url = f"http://127.0.0.1:{port}/existing.txt"

            new = self._write_manifest("new.txt", [
                make_manifest_line(HASH_B, "changed.tar.zst"),
            ])
            result = self._run_merge(new, "--existing", url, "--strict")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("hash changed", result.stderr)
        finally:
            server.shutdown()
            server_thread.join(timeout=5)
            server.server_close()

    def test_existing_http_unreachable_errors(self):
        """--existing with unreachable HTTP URL errors gracefully."""
        new = self._write_manifest("new.txt", [
            make_manifest_line(HASH_A, "pkg.tar.zst"),
        ])
        result = self._run_merge(
            new, "--existing", "http://127.0.0.1:1/nonexistent.txt"
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("failed to fetch", result.stderr)

    # --- --retain flag ---

    def _write_retain(self, name, paths):
        """Write a retain list file (one path per line) and return its path."""
        path = self.test_dir / name
        path.write_text("\n".join(paths) + "\n", encoding="utf-8")
        return path

    def test_retain_prunes_stale_existing(self):
        """--retain prunes existing entries not in retain list."""
        existing = self._write_manifest("existing.txt", [
            make_manifest_line(HASH_A, "old.tar.zst"),
            make_manifest_line(HASH_B, "stale.tar.zst"),
        ])
        new = self._write_manifest("new.txt", [
            make_manifest_line(HASH_C, "new.tar.zst"),
        ])
        retain = self._write_retain("retain.txt", ["old.tar.zst"])

        result = self._run_merge(
            new, "--existing", existing, "--retain", retain
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        entries = self._parse_output(result.stdout)
        paths = {e[1] for e in entries}
        self.assertEqual(len(entries), 2)
        self.assertIn("old.tar.zst", paths)
        self.assertIn("new.tar.zst", paths)
        self.assertNotIn("stale.tar.zst", paths)

    def test_retain_preserves_new_entries_not_in_retain(self):
        """New manifest entries survive even if absent from retain list."""
        new = self._write_manifest("new.txt", [
            make_manifest_line(HASH_A, "brand-new.tar.zst"),
        ])
        retain = self._write_retain("retain.txt", ["something-else.tar.zst"])

        result = self._run_merge(new, "--retain", retain)
        self.assertEqual(result.returncode, 0, result.stderr)
        entries = self._parse_output(result.stdout)
        self.assertEqual(len(entries), 1)
        self.assertEqual(entries[0][1], "brand-new.tar.zst")

    def test_retain_without_existing(self):
        """--retain without --existing keeps all new entries."""
        new = self._write_manifest("new.txt", [
            make_manifest_line(HASH_A, "a.tar.zst"),
            make_manifest_line(HASH_B, "b.tar.zst"),
        ])
        retain = self._write_retain("retain.txt", ["a.tar.zst"])

        result = self._run_merge(new, "--retain", retain)
        self.assertEqual(result.returncode, 0, result.stderr)
        entries = self._parse_output(result.stdout)
        self.assertEqual(len(entries), 2)

    def test_retain_empty_prunes_all_existing(self):
        """Empty retain list prunes all existing-only entries."""
        existing = self._write_manifest("existing.txt", [
            make_manifest_line(HASH_A, "old.tar.zst"),
            make_manifest_line(HASH_B, "ancient.tar.zst"),
        ])
        new = self._write_manifest("new.txt", [
            make_manifest_line(HASH_C, "new.tar.zst"),
        ])
        retain = self._write_retain("retain.txt", [])

        result = self._run_merge(
            new, "--existing", existing, "--retain", retain
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        entries = self._parse_output(result.stdout)
        self.assertEqual(len(entries), 1)
        self.assertEqual(entries[0][1], "new.tar.zst")

    def test_retain_with_comments_and_blanks(self):
        """Retain file comments and blank lines are skipped."""
        existing = self._write_manifest("existing.txt", [
            make_manifest_line(HASH_A, "kept.tar.zst"),
            make_manifest_line(HASH_B, "pruned.tar.zst"),
        ])
        new = self._write_manifest("new.txt", [
            make_manifest_line(HASH_C, "new.tar.zst"),
        ])
        retain_path = self.test_dir / "retain.txt"
        retain_path.write_text(
            "# comment\n\nkept.tar.zst\n# another\n\n", encoding="utf-8"
        )

        result = self._run_merge(
            new, "--existing", existing, "--retain", retain_path
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        entries = self._parse_output(result.stdout)
        paths = {e[1] for e in entries}
        self.assertEqual(len(entries), 2)
        self.assertIn("kept.tar.zst", paths)
        self.assertIn("new.tar.zst", paths)

    def test_retain_http_url(self):
        """--retain fetches retain list from HTTP URL."""
        serve_dir = self.test_dir / "serve"
        serve_dir.mkdir()
        (serve_dir / "retain.txt").write_text(
            "old.tar.zst\n", encoding="utf-8"
        )

        existing = self._write_manifest("existing.txt", [
            make_manifest_line(HASH_A, "old.tar.zst"),
            make_manifest_line(HASH_B, "stale.tar.zst"),
        ])
        new = self._write_manifest("new.txt", [
            make_manifest_line(HASH_C, "new.tar.zst"),
        ])

        handler = partial(_QuietHTTPHandler, directory=str(serve_dir))
        server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
        server_thread = threading.Thread(target=server.serve_forever, daemon=True)
        server_thread.start()
        try:
            port = server.server_address[1]
            url = f"http://127.0.0.1:{port}/retain.txt"

            result = self._run_merge(
                new, "--existing", existing, "--retain", url
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            entries = self._parse_output(result.stdout)
            paths = {e[1] for e in entries}
            self.assertEqual(len(entries), 2)
            self.assertIn("old.tar.zst", paths)
            self.assertIn("new.tar.zst", paths)
            self.assertNotIn("stale.tar.zst", paths)
        finally:
            server.shutdown()
            server_thread.join(timeout=5)
            server.server_close()

    def test_retain_http_unreachable_errors(self):
        """--retain with unreachable HTTP URL errors gracefully."""
        new = self._write_manifest("new.txt", [
            make_manifest_line(HASH_A, "pkg.tar.zst"),
        ])
        result = self._run_merge(
            new, "--retain", "http://127.0.0.1:1/nonexistent.txt"
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("failed to fetch", result.stderr)

    def test_retain_nonexistent_local_file_errors(self):
        """--retain with nonexistent local path errors at runtime."""
        new = self._write_manifest("new.txt", [
            make_manifest_line(HASH_A, "pkg.tar.zst"),
        ])
        result = self._run_merge(new, "--retain", "/nonexistent/retain.txt")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not found", result.stderr)

    # --- --retain-prefix flag ---

    def test_retain_prefix_matches_prefixed_entries(self):
        """--retain-prefix prepends to retain entries so they match depot paths."""
        existing = self._write_manifest("existing.txt", [
            make_manifest_line(HASH_A, "s3://bucket/old.tar.zst"),
        ])
        new = self._write_manifest("new.txt", [
            make_manifest_line(HASH_B, "s3://bucket/new.tar.zst"),
        ])
        retain = self._write_retain("retain.txt", ["old.tar.zst"])

        result = self._run_merge(
            new, "--existing", existing,
            "--retain", retain, "--retain-prefix", "s3://bucket/"
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        entries = self._parse_output(result.stdout)
        paths = {e[1] for e in entries}
        self.assertIn("s3://bucket/old.tar.zst", paths)
        self.assertIn("s3://bucket/new.tar.zst", paths)

    def test_retain_prefix_prunes_unmatched(self):
        """--retain-prefix prunes existing entries not in prefixed retain list."""
        existing = self._write_manifest("existing.txt", [
            make_manifest_line(HASH_A, "s3://bucket/old.tar.zst"),
            make_manifest_line(HASH_B, "s3://bucket/stale.tar.zst"),
        ])
        new = self._write_manifest("new.txt", [
            make_manifest_line(HASH_C, "s3://bucket/new.tar.zst"),
        ])
        retain = self._write_retain("retain.txt", ["old.tar.zst"])

        result = self._run_merge(
            new, "--existing", existing,
            "--retain", retain, "--retain-prefix", "s3://bucket/"
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        entries = self._parse_output(result.stdout)
        paths = {e[1] for e in entries}
        self.assertEqual(len(entries), 2)
        self.assertIn("s3://bucket/old.tar.zst", paths)
        self.assertIn("s3://bucket/new.tar.zst", paths)
        self.assertNotIn("s3://bucket/stale.tar.zst", paths)

    def test_retain_prefix_without_retain_errors(self):
        """--retain-prefix without --retain or --retain-s3-ls errors at runtime."""
        new = self._write_manifest("new.txt", [
            make_manifest_line(HASH_A, "pkg.tar.zst"),
        ])
        result = self._run_merge(new, "--retain-prefix", "s3://bucket/")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--retain-prefix requires", result.stderr)

    def test_retain_prefix_empty_string(self):
        """--retain-prefix with empty string is a no-op (entries match as-is)."""
        existing = self._write_manifest("existing.txt", [
            make_manifest_line(HASH_A, "old.tar.zst"),
            make_manifest_line(HASH_B, "stale.tar.zst"),
        ])
        new = self._write_manifest("new.txt", [
            make_manifest_line(HASH_C, "new.tar.zst"),
        ])
        retain = self._write_retain("retain.txt", ["old.tar.zst"])

        result = self._run_merge(
            new, "--existing", existing,
            "--retain", retain, "--retain-prefix", ""
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        entries = self._parse_output(result.stdout)
        paths = {e[1] for e in entries}
        self.assertEqual(len(entries), 2)
        self.assertIn("old.tar.zst", paths)
        self.assertIn("new.tar.zst", paths)
        self.assertNotIn("stale.tar.zst", paths)

    def test_retain_prefix_preserves_new_entries(self):
        """New manifest entries survive even when absent from prefixed retain."""
        new = self._write_manifest("new.txt", [
            make_manifest_line(HASH_A, "s3://bucket/brand-new.tar.zst"),
        ])
        retain = self._write_retain("retain.txt", ["something-else.tar.zst"])

        result = self._run_merge(
            new, "--retain", retain, "--retain-prefix", "s3://bucket/"
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        entries = self._parse_output(result.stdout)
        self.assertEqual(len(entries), 1)
        self.assertEqual(entries[0][1], "s3://bucket/brand-new.tar.zst")

    def test_retain_prefix_with_http_retain(self):
        """--retain-prefix works with remote retain list."""
        serve_dir = self.test_dir / "serve"
        serve_dir.mkdir()
        (serve_dir / "retain.txt").write_text(
            "old.tar.zst\n", encoding="utf-8"
        )

        existing = self._write_manifest("existing.txt", [
            make_manifest_line(HASH_A, "s3://bucket/old.tar.zst"),
            make_manifest_line(HASH_B, "s3://bucket/stale.tar.zst"),
        ])
        new = self._write_manifest("new.txt", [
            make_manifest_line(HASH_C, "s3://bucket/new.tar.zst"),
        ])

        handler = partial(_QuietHTTPHandler, directory=str(serve_dir))
        server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
        server_thread = threading.Thread(target=server.serve_forever, daemon=True)
        server_thread.start()
        try:
            port = server.server_address[1]
            url = f"http://127.0.0.1:{port}/retain.txt"

            result = self._run_merge(
                new, "--existing", existing,
                "--retain", url, "--retain-prefix", "s3://bucket/"
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            entries = self._parse_output(result.stdout)
            paths = {e[1] for e in entries}
            self.assertEqual(len(entries), 2)
            self.assertIn("s3://bucket/old.tar.zst", paths)
            self.assertIn("s3://bucket/new.tar.zst", paths)
            self.assertNotIn("s3://bucket/stale.tar.zst", paths)
        finally:
            server.shutdown()
            server_thread.join(timeout=5)
            server.server_close()

    # --- --retain-s3-ls flag ---

    def _write_s3_ls(self, name, keys):
        """Write a file in 'aws s3 ls' format and return its path."""
        path = self.test_dir / name
        lines = [f"2024-01-15 12:34:56       1234 {key}\n" for key in keys]
        path.write_text("".join(lines), encoding="utf-8")
        return path

    def test_retain_s3_ls_basic(self):
        """--retain-s3-ls parses aws s3 ls format to build retain set."""
        existing = self._write_manifest("existing.txt", [
            make_manifest_line(HASH_A, "old.tar.zst"),
            make_manifest_line(HASH_B, "stale.tar.zst"),
        ])
        new = self._write_manifest("new.txt", [
            make_manifest_line(HASH_C, "new.tar.zst"),
        ])
        s3_ls = self._write_s3_ls("s3ls.txt", ["old.tar.zst"])

        result = self._run_merge(
            new, "--existing", existing, "--retain-s3-ls", s3_ls
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        entries = self._parse_output(result.stdout)
        paths = {e[1] for e in entries}
        self.assertEqual(len(entries), 2)
        self.assertIn("old.tar.zst", paths)
        self.assertIn("new.tar.zst", paths)
        self.assertNotIn("stale.tar.zst", paths)

    def test_retain_s3_ls_skips_pre_lines(self):
        """PRE lines in aws s3 ls output are ignored."""
        existing = self._write_manifest("existing.txt", [
            make_manifest_line(HASH_A, "old.tar.zst"),
            make_manifest_line(HASH_B, "stale.tar.zst"),
        ])
        new = self._write_manifest("new.txt", [
            make_manifest_line(HASH_C, "new.tar.zst"),
        ])
        s3_ls_path = self.test_dir / "s3ls.txt"
        s3_ls_path.write_text(
            "                           PRE subdir/\n"
            "2024-01-15 12:34:56       1234 old.tar.zst\n",
            encoding="utf-8",
        )

        result = self._run_merge(
            new, "--existing", existing, "--retain-s3-ls", s3_ls_path
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        entries = self._parse_output(result.stdout)
        paths = {e[1] for e in entries}
        self.assertEqual(len(entries), 2)
        self.assertIn("old.tar.zst", paths)
        self.assertIn("new.tar.zst", paths)

    def test_retain_s3_ls_with_retain_prefix(self):
        """--retain-s3-ls combined with --retain-prefix matches prefixed paths."""
        existing = self._write_manifest("existing.txt", [
            make_manifest_line(HASH_A, "s3://bucket/old.tar.zst"),
            make_manifest_line(HASH_B, "s3://bucket/stale.tar.zst"),
        ])
        new = self._write_manifest("new.txt", [
            make_manifest_line(HASH_C, "s3://bucket/new.tar.zst"),
        ])
        s3_ls = self._write_s3_ls("s3ls.txt", ["old.tar.zst"])

        result = self._run_merge(
            new, "--existing", existing,
            "--retain-s3-ls", s3_ls, "--retain-prefix", "s3://bucket/"
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        entries = self._parse_output(result.stdout)
        paths = {e[1] for e in entries}
        self.assertEqual(len(entries), 2)
        self.assertIn("s3://bucket/old.tar.zst", paths)
        self.assertIn("s3://bucket/new.tar.zst", paths)
        self.assertNotIn("s3://bucket/stale.tar.zst", paths)

    def test_retain_and_retain_s3_ls_mutually_exclusive(self):
        """--retain and --retain-s3-ls together is rejected."""
        new = self._write_manifest("new.txt", [
            make_manifest_line(HASH_A, "pkg.tar.zst"),
        ])
        retain = self._write_retain("retain.txt", ["pkg.tar.zst"])
        s3_ls = self._write_s3_ls("s3ls.txt", ["pkg.tar.zst"])

        result = self._run_merge(
            new, "--retain", retain, "--retain-s3-ls", s3_ls
        )
        self.assertNotEqual(result.returncode, 0)

    def test_retain_s3_ls_preserves_new_entries(self):
        """New manifest entries survive even if absent from s3 ls retain."""
        new = self._write_manifest("new.txt", [
            make_manifest_line(HASH_A, "brand-new.tar.zst"),
        ])
        s3_ls = self._write_s3_ls("s3ls.txt", ["something-else.tar.zst"])

        result = self._run_merge(new, "--retain-s3-ls", s3_ls)
        self.assertEqual(result.returncode, 0, result.stderr)
        entries = self._parse_output(result.stdout)
        self.assertEqual(len(entries), 1)
        self.assertEqual(entries[0][1], "brand-new.tar.zst")

    def test_retain_s3_ls_realistic_format(self):
        """--retain-s3-ls handles real aws s3 ls whitespace and size widths."""
        existing = self._write_manifest("existing.txt", [
            make_manifest_line(HASH_A, "toolchain@r0-darwin-arm64.tar.zst"),
            make_manifest_line(HASH_B, "sdk@r0-linux-x86_64.tar.zst"),
            make_manifest_line(HASH_C, "stale@r0-darwin-arm64.tar.zst"),
        ])
        new = self._write_manifest("new.txt", [
            make_manifest_line(HASH_D, "newpkg@r0-darwin-arm64.tar.zst"),
        ])
        s3_ls_path = self.test_dir / "s3ls.txt"
        # Realistic aws s3 ls output: varying size widths, right-aligned
        s3_ls_path.write_text(
            "2026-03-29 01:02:49  182643143 toolchain@r0-darwin-arm64.tar.zst\n"
            "2026-03-29 11:17:13   67997829 sdk@r0-linux-x86_64.tar.zst\n"
            "2026-03-29 11:17:20       7561 packages.txt\n",
            encoding="utf-8",
        )

        result = self._run_merge(
            new, "--existing", existing, "--retain-s3-ls", s3_ls_path
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        entries = self._parse_output(result.stdout)
        paths = {e[1] for e in entries}
        self.assertEqual(len(entries), 3)
        self.assertIn("toolchain@r0-darwin-arm64.tar.zst", paths)
        self.assertIn("sdk@r0-linux-x86_64.tar.zst", paths)
        self.assertIn("newpkg@r0-darwin-arm64.tar.zst", paths)
        self.assertNotIn("stale@r0-darwin-arm64.tar.zst", paths)
        self.assertNotIn("packages.txt", paths)

    def test_retain_s3_ls_nonexistent_file_errors(self):
        """--retain-s3-ls with nonexistent local file errors at runtime."""
        new = self._write_manifest("new.txt", [
            make_manifest_line(HASH_A, "pkg.tar.zst"),
        ])
        result = self._run_merge(
            new, "--retain-s3-ls", "/nonexistent/s3ls.txt"
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not found", result.stderr)

    def test_retain_s3_ls_http_url(self):
        """--retain-s3-ls fetches retain list from HTTP URL."""
        serve_dir = self.test_dir / "serve"
        serve_dir.mkdir()
        (serve_dir / "s3ls.txt").write_text(
            "2024-01-15 12:34:56       1234 old.tar.zst\n",
            encoding="utf-8",
        )

        existing = self._write_manifest("existing.txt", [
            make_manifest_line(HASH_A, "old.tar.zst"),
            make_manifest_line(HASH_B, "stale.tar.zst"),
        ])
        new = self._write_manifest("new.txt", [
            make_manifest_line(HASH_C, "new.tar.zst"),
        ])

        handler = partial(_QuietHTTPHandler, directory=str(serve_dir))
        server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
        server_thread = threading.Thread(target=server.serve_forever, daemon=True)
        server_thread.start()
        try:
            port = server.server_address[1]
            url = f"http://127.0.0.1:{port}/s3ls.txt"

            result = self._run_merge(
                new, "--existing", existing, "--retain-s3-ls", url
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            entries = self._parse_output(result.stdout)
            paths = {e[1] for e in entries}
            self.assertEqual(len(entries), 2)
            self.assertIn("old.tar.zst", paths)
            self.assertIn("new.tar.zst", paths)
            self.assertNotIn("stale.tar.zst", paths)
        finally:
            server.shutdown()
            server_thread.join(timeout=5)
            server.server_close()
