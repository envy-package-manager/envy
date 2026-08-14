"""Functional tests for package depot feature.

Tests export --depot-prefix, depot manifest parsing, depot-based sync,
error handling, and graceful fallback to source builds.
"""

import hashlib
import io
import os
import shutil
import socket
import tarfile
import tempfile
import threading
import time
import unittest
from functools import partial
from http.server import (
    BaseHTTPRequestHandler,
    SimpleHTTPRequestHandler,
    ThreadingHTTPServer,
)
from pathlib import Path

from . import test_config
from .env import EnvyTestCase
from .test_config import make_manifest, parse_export_line
from .trace_parser import TraceParser

TEST_ARCHIVE_FILES = {
    "root/file1.txt": "Test file content\n",
}


def _create_test_archive(output_path: Path) -> str:
    """Create test.tar.gz archive and return its SHA256 hash."""
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz") as tar:
        for name, content in TEST_ARCHIVE_FILES.items():
            data = content.encode("utf-8")
            info = tarfile.TarInfo(name=name)
            info.size = len(data)
            tar.addfile(info, io.BytesIO(data))
    archive_data = buf.getvalue()
    output_path.write_bytes(archive_data)
    return hashlib.sha256(archive_data).hexdigest()


def _get_free_port():
    """Get a TCP port that nothing is currently listening on."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class _QuietHandler(SimpleHTTPRequestHandler):
    """HTTP handler that suppresses per-request logging."""

    def __init__(self, *args, directory=None, **kwargs):
        super().__init__(*args, directory=directory, **kwargs)

    def log_message(self, format, *args):
        return


def _spec_content(identity, archive_path, archive_hash):
    """Generate a cache-managed EXPORTABLE spec."""
    p = archive_path.as_posix()
    return f'''IDENTITY = "{identity}"
EXPORTABLE = true

FETCH = {{
  source = "{p}",
  sha256 = "{archive_hash}"
}}

STAGE = {{strip = 1}}

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  if envy.PLATFORM == "windows" then
    envy.run([[Set-Content -Path built.txt -Value "built"]], {{ shell = ENVY_SHELL.POWERSHELL }})
  else
    envy.run([[echo 'built' > built.txt]])
  end
end
'''


class TestExportDepotPrefix(EnvyTestCase):
    """Tests for 'envy export --depot-prefix' flag."""

    def setUp(self):
        super().setUp()
        self.test_dir = self.make_temp_dir("test_dir")
        self.output_dir = self.make_temp_dir("output_dir")

        self.archive_path = self.test_dir / "test.tar.gz"
        self.archive_hash = _create_test_archive(self.archive_path)

        for name, identity in [
            ("pkg_a.lua", "local.depot_a@v1"),
            ("pkg_b.lua", "local.depot_b@v1"),
        ]:
            (self.test_dir / name).write_text(
                _spec_content(identity, self.archive_path, self.archive_hash),
                encoding="utf-8",
            )

    def tearDown(self):
        for d in [self.cache_root, self.test_dir, self.output_dir]:
            shutil.rmtree(d, ignore_errors=True)

    def _run(self, *args):
        cmd = [str(self.envy), "--cache-root", str(self.cache_root), *args]
        return test_config.run(
            cmd, cwd=self.project_root, capture_output=True, text=True
        )

    def _make_manifest(self, specs):
        packages = ", ".join(
            f'{{ spec = "{s}", source = "{self.test_dir.as_posix()}/{f}" }}'
            for s, f in specs
        )
        path = self.test_dir / "envy.lua"
        path.write_text(
            make_manifest(f"\nPACKAGES = {{\n    {packages}\n}}\n"),
            encoding="utf-8",
        )
        return path

    def test_export_depot_prefix_always_sha256(self):
        """Export with --depot-prefix always outputs SHA256 + prefixed URLs."""
        m = self._make_manifest([("local.depot_a@v1", "pkg_a.lua")])
        r = self._run("install", "--manifest", str(m))
        self.assertEqual(r.returncode, 0, f"install failed: {r.stderr}")

        r = self._run(
            "export",
            "local.depot_a@v1",
            "-o",
            str(self.output_dir),
            "--depot-prefix",
            "s3://bucket/cache/",
            "--manifest",
            str(m),
        )
        self.assertEqual(r.returncode, 0, f"export failed: {r.stderr}")

        lines = [l for l in r.stdout.strip().split("\n") if l.strip()]
        self.assertEqual(len(lines), 1)
        # Format: <64hex>  <prefix><filename>
        parts = lines[0].split("  ", 1)
        self.assertEqual(len(parts), 2)
        self.assertEqual(len(parts[0]), 64)
        self.assertTrue(parts[1].startswith("s3://bucket/cache/"))
        self.assertTrue(parts[1].endswith(".tar.zst"))
        self.assertIn("local.depot_a@v1-", parts[1])

    def test_export_depot_prefix_multiple_packages(self):
        """Export multiple packages with --depot-prefix, all with SHA256."""
        m = self._make_manifest(
            [
                ("local.depot_a@v1", "pkg_a.lua"),
                ("local.depot_b@v1", "pkg_b.lua"),
            ]
        )
        r = self._run("install", "--manifest", str(m))
        self.assertEqual(r.returncode, 0, f"install failed: {r.stderr}")

        r = self._run(
            "export",
            "-o",
            str(self.output_dir),
            "--depot-prefix",
            "https://cdn.example.com/pkgs/",
            "--manifest",
            str(m),
        )
        self.assertEqual(r.returncode, 0, f"export failed: {r.stderr}")

        lines = [l for l in r.stdout.strip().split("\n") if l.strip()]
        self.assertEqual(len(lines), 2)
        for line in lines:
            parts = line.split("  ", 1)
            self.assertEqual(len(parts), 2)
            self.assertEqual(len(parts[0]), 64)
            self.assertTrue(parts[1].startswith("https://cdn.example.com/pkgs/"))
            self.assertTrue(parts[1].endswith(".tar.zst"))


class TestPackageDepot(EnvyTestCase):
    """Tests for depot-based sync: depot manifests, archive lookup, fallback."""

    def setUp(self):
        self.source_cache = self.make_temp_dir("source_cache")
        self.target_cache = self.make_temp_dir("target_cache")
        self.test_dir = self.make_temp_dir("test_dir")
        self.output_dir = self.make_temp_dir("output_dir")
        self.serve_dir = self.make_temp_dir("serve_dir")
        self.envy = test_config.get_envy_executable()
        self.project_root = Path(__file__).parent.parent

        self.archive_path = self.test_dir / "test.tar.gz"
        self.archive_hash = _create_test_archive(self.archive_path)

        self.spec_lua = {}
        for name, identity in [
            ("pkg_a.lua", "local.depot_a@v1"),
            ("pkg_b.lua", "local.depot_b@v1"),
        ]:
            (self.test_dir / name).write_text(
                _spec_content(identity, self.archive_path, self.archive_hash),
                encoding="utf-8",
            )
            self.spec_lua[identity] = f"{self.test_dir.as_posix()}/{name}"

    def tearDown(self):
        for d in [
            self.source_cache,
            self.target_cache,
            self.test_dir,
            self.output_dir,
            self.serve_dir,
        ]:
            shutil.rmtree(d, ignore_errors=True)

    # -- helpers --

    def _run(self, *args, cache_root=None):
        cache = cache_root or self.source_cache
        cmd = [str(self.envy), "--cache-root", str(cache), *args]
        return test_config.run(
            cmd, cwd=self.project_root, capture_output=True, text=True
        )

    def _make_source_manifest(self, identities):
        """Create manifest for initial install/export (no depot directives)."""
        packages = ", ".join(
            f'{{ spec = "{i}", source = "{self.spec_lua[i]}" }}' for i in identities
        )
        path = self.test_dir / "source.lua"
        path.write_text(
            make_manifest(f"\nPACKAGES = {{\n    {packages}\n}}\n"),
            encoding="utf-8",
        )
        return path

    def _install_and_export(self, identities):
        """Install + export packages from source cache. Returns archive paths."""
        m = self._make_source_manifest(identities)
        r = self._run("install", "--manifest", str(m))
        self.assertEqual(r.returncode, 0, f"install failed: {r.stderr}")

        r = self._run("export", "-o", str(self.output_dir), "--manifest", str(m))
        self.assertEqual(r.returncode, 0, f"export failed: {r.stderr}")
        paths = []
        for line in r.stdout.strip().split("\n"):
            line = line.strip()
            if not line:
                continue
            _, p = parse_export_line(line)
            paths.append(p)
        return paths

    def _start_server(self):
        """Start HTTP server on self.serve_dir. Returns (server, port)."""
        handler = partial(_QuietHandler, directory=str(self.serve_dir))
        srv = ThreadingHTTPServer(("127.0.0.1", 0), handler)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        return srv, srv.server_address[1]

    def _make_depot_manifest(self, archive_paths, port, name="depot.txt"):
        """Copy archives to serve_dir, write SHA256 depot manifest. Returns URL."""
        lines = []
        for ap in archive_paths:
            shutil.copy2(ap, self.serve_dir / ap.name)
            h = hashlib.sha256(ap.read_bytes()).hexdigest()
            lines.append(f"{h}  http://127.0.0.1:{port}/{ap.name}")
        (self.serve_dir / name).write_text("\n".join(lines) + "\n", encoding="utf-8")
        return f"http://127.0.0.1:{port}/{name}"

    def _make_target_manifest(self, identities, depot_urls):
        """Create manifest with a PACKAGE_DEPOTS global for target sync."""
        header = '-- @envy bin "envy-bin"\n'
        packages = ", ".join(
            f'{{ spec = "{i}", source = "{self.spec_lua[i]}" }}' for i in identities
        )
        depots = ", ".join(f'"{u}"' for u in depot_urls)
        path = self.test_dir / "target.lua"
        path.write_text(
            header
            + f"\nPACKAGES = {{\n    {packages}\n}}\n"
            + f"PACKAGE_DEPOTS = {{ {depots} }}\n",
            encoding="utf-8",
        )
        return path

    # -- tests --

    def test_depot_hit_skips_build(self):
        """Sync with depot archive uses pre-built package."""
        archives = self._install_and_export(["local.depot_a@v1"])
        self.assertEqual(len(archives), 1)

        srv, port = self._start_server()
        try:
            depot_url = self._make_depot_manifest(archives, port)
            m = self._make_target_manifest(["local.depot_a@v1"], [depot_url])

            r = self._run("sync", "--manifest", str(m), cache_root=self.target_cache)
            self.assertEqual(r.returncode, 0, f"sync failed: {r.stderr}")

            pkg_dir = self.target_cache / "packages" / "local.depot_a@v1"
            self.assertTrue(pkg_dir.exists(), "Package should be in target cache")
        finally:
            srv.shutdown()
            srv.server_close()

    def test_depot_hit_emits_depot_check(self):
        """A depot hit records a depot_check trace event with result=hit."""
        archives = self._install_and_export(["local.depot_a@v1"])
        self.assertEqual(len(archives), 1)

        srv, port = self._start_server()
        try:
            depot_url = self._make_depot_manifest(archives, port)
            m = self._make_target_manifest(["local.depot_a@v1"], [depot_url])

            trace = self.test_dir / "depot.jsonl"
            r = self._run(
                f"--trace=file:{trace}",
                "sync",
                "--manifest",
                str(m),
                cache_root=self.target_cache,
            )
            self.assertEqual(r.returncode, 0, f"sync failed: {r.stderr}")

            checks = TraceParser(trace).filter_by_event("depot_check")
            hits = [e for e in checks if e.raw.get("result") == "hit"]
            self.assertTrue(hits, f"expected a depot_check hit: {checks}")
            self.assertEqual(hits[0].spec, "local.depot_a@v1")
        finally:
            srv.shutdown()
            srv.server_close()

    def test_depot_hit_multiple_packages(self):
        """Multiple packages all served from depot."""
        archives = self._install_and_export(["local.depot_a@v1", "local.depot_b@v1"])
        self.assertEqual(len(archives), 2)

        srv, port = self._start_server()
        try:
            depot_url = self._make_depot_manifest(archives, port)
            m = self._make_target_manifest(
                ["local.depot_a@v1", "local.depot_b@v1"], [depot_url]
            )

            r = self._run("sync", "--manifest", str(m), cache_root=self.target_cache)
            self.assertEqual(r.returncode, 0, f"sync failed: {r.stderr}")

            for pkg in ["local.depot_a@v1", "local.depot_b@v1"]:
                pkg_dir = self.target_cache / "packages" / pkg
                self.assertTrue(pkg_dir.exists(), f"{pkg} should be cached")
        finally:
            srv.shutdown()
            srv.server_close()

    def test_depot_manifest_unreachable(self):
        """Sync succeeds when depot manifest is unreachable (builds from source)."""
        dead_port = _get_free_port()
        m = self._make_target_manifest(
            ["local.depot_a@v1"],
            [f"http://127.0.0.1:{dead_port}/depot.txt"],
        )

        r = self._run("sync", "--manifest", str(m), cache_root=self.target_cache)
        self.assertEqual(r.returncode, 0, f"sync failed: {r.stderr}")

        pkg_dir = self.target_cache / "packages" / "local.depot_a@v1"
        self.assertTrue(pkg_dir.exists())

    def test_depot_manifest_empty(self):
        """Sync succeeds with empty depot manifest (builds from source)."""
        srv, port = self._start_server()
        try:
            (self.serve_dir / "depot.txt").write_text("", encoding="utf-8")
            depot_url = f"http://127.0.0.1:{port}/depot.txt"
            m = self._make_target_manifest(["local.depot_a@v1"], [depot_url])

            r = self._run("sync", "--manifest", str(m), cache_root=self.target_cache)
            self.assertEqual(r.returncode, 0, f"sync failed: {r.stderr}")

            pkg_dir = self.target_cache / "packages" / "local.depot_a@v1"
            self.assertTrue(pkg_dir.exists())
        finally:
            srv.shutdown()
            srv.server_close()

    def test_depot_archive_not_found(self):
        """Sync falls back to source when depot archive returns 404."""
        archives = self._install_and_export(["local.depot_a@v1"])

        srv, port = self._start_server()
        try:
            # Write SHA256 depot manifest but don't copy archive to serve_dir
            h = hashlib.sha256(archives[0].read_bytes()).hexdigest()
            (self.serve_dir / "depot.txt").write_text(
                f"{h}  http://127.0.0.1:{port}/{archives[0].name}\n",
                encoding="utf-8",
            )
            depot_url = f"http://127.0.0.1:{port}/depot.txt"
            m = self._make_target_manifest(["local.depot_a@v1"], [depot_url])

            r = self._run("sync", "--manifest", str(m), cache_root=self.target_cache)
            self.assertEqual(r.returncode, 0, f"sync failed: {r.stderr}")

            pkg_dir = self.target_cache / "packages" / "local.depot_a@v1"
            self.assertTrue(pkg_dir.exists())
        finally:
            srv.shutdown()
            srv.server_close()

    def test_depot_mixed_hit_and_miss(self):
        """One package from depot, the other from source. Both succeed."""
        archives = self._install_and_export(["local.depot_a@v1", "local.depot_b@v1"])
        self.assertEqual(len(archives), 2)

        srv, port = self._start_server()
        try:
            # Only serve pkg_a via depot
            pkg_a_archives = [a for a in archives if "depot_a" in a.name]
            self.assertEqual(len(pkg_a_archives), 1)
            depot_url = self._make_depot_manifest(pkg_a_archives, port)

            m = self._make_target_manifest(
                ["local.depot_a@v1", "local.depot_b@v1"], [depot_url]
            )

            r = self._run("sync", "--manifest", str(m), cache_root=self.target_cache)
            self.assertEqual(r.returncode, 0, f"sync failed: {r.stderr}")

            for pkg in ["local.depot_a@v1", "local.depot_b@v1"]:
                pkg_dir = self.target_cache / "packages" / pkg
                self.assertTrue(pkg_dir.exists(), f"{pkg} should be cached")
        finally:
            srv.shutdown()
            srv.server_close()

    def test_multiple_depots_merged(self):
        """Two depot manifests with disjoint packages both contribute."""
        archives = self._install_and_export(["local.depot_a@v1", "local.depot_b@v1"])
        a_archives = [a for a in archives if "depot_a" in a.name]
        b_archives = [a for a in archives if "depot_b" in a.name]

        srv, port = self._start_server()
        try:
            depot_a_url = self._make_depot_manifest(a_archives, port, "depot_a.txt")
            depot_b_url = self._make_depot_manifest(b_archives, port, "depot_b.txt")

            m = self._make_target_manifest(
                ["local.depot_a@v1", "local.depot_b@v1"],
                [depot_a_url, depot_b_url],
            )

            r = self._run("sync", "--manifest", str(m), cache_root=self.target_cache)
            self.assertEqual(r.returncode, 0, f"sync failed: {r.stderr}")

            for pkg in ["local.depot_a@v1", "local.depot_b@v1"]:
                pkg_dir = self.target_cache / "packages" / pkg
                self.assertTrue(pkg_dir.exists(), f"{pkg} should be cached")
        finally:
            srv.shutdown()
            srv.server_close()

    def test_full_ci_workflow(self):
        """End-to-end: export with --depot-prefix, serve, sync from depot."""
        source_m = self._make_source_manifest(["local.depot_a@v1"])
        r = self._run("install", "--manifest", str(source_m))
        self.assertEqual(r.returncode, 0, f"install failed: {r.stderr}")

        srv, port = self._start_server()
        try:
            prefix = f"http://127.0.0.1:{port}/"
            r = self._run(
                "export",
                "-o",
                str(self.serve_dir),
                "--depot-prefix",
                prefix,
                "--manifest",
                str(source_m),
            )
            self.assertEqual(r.returncode, 0, f"export failed: {r.stderr}")

            # stdout lines are SHA256 depot manifest content (always now)
            lines = [l for l in r.stdout.strip().split("\n") if l.strip()]
            self.assertEqual(len(lines), 1)
            parts = lines[0].split("  ", 1)
            self.assertEqual(len(parts), 2, "Export must produce SHA256 format")
            self.assertEqual(len(parts[0]), 64)

            (self.serve_dir / "depot.txt").write_text(
                r.stdout.strip() + "\n", encoding="utf-8"
            )
            depot_url = f"http://127.0.0.1:{port}/depot.txt"

            m = self._make_target_manifest(["local.depot_a@v1"], [depot_url])
            r = self._run("sync", "--manifest", str(m), cache_root=self.target_cache)
            self.assertEqual(r.returncode, 0, f"sync failed: {r.stderr}")

            pkg_dir = self.target_cache / "packages" / "local.depot_a@v1"
            self.assertTrue(pkg_dir.exists())
        finally:
            srv.shutdown()
            srv.server_close()

    def test_depot_then_local_cache_hit(self):
        """Second sync gets local cache hit; no depot fetch needed."""
        archives = self._install_and_export(["local.depot_a@v1"])

        srv, port = self._start_server()
        depot_url = self._make_depot_manifest(archives, port)
        m = self._make_target_manifest(["local.depot_a@v1"], [depot_url])

        try:
            r = self._run("sync", "--manifest", str(m), cache_root=self.target_cache)
            self.assertEqual(r.returncode, 0, f"first sync failed: {r.stderr}")
        finally:
            srv.shutdown()
            srv.server_close()

        # Second sync with server down: local cache hit
        r = self._run("sync", "--manifest", str(m), cache_root=self.target_cache)
        self.assertEqual(r.returncode, 0, f"second sync failed: {r.stderr}")

    def test_sha256_wrong_hash_falls_back(self):
        """SHA256 mismatch -> fallback to source build."""
        archives = self._install_and_export(["local.depot_a@v1"])

        srv, port = self._start_server()
        try:
            shutil.copy2(archives[0], self.serve_dir / archives[0].name)
            wrong_hash = "0" * 64
            content = f"{wrong_hash}  http://127.0.0.1:{port}/{archives[0].name}\n"
            (self.serve_dir / "depot.txt").write_text(content, encoding="utf-8")
            depot_url = f"http://127.0.0.1:{port}/depot.txt"
            m = self._make_target_manifest(["local.depot_a@v1"], [depot_url])

            r = self._run("sync", "--manifest", str(m), cache_root=self.target_cache)
            self.assertEqual(r.returncode, 0, f"sync failed: {r.stderr}")

            pkg_dir = self.target_cache / "packages" / "local.depot_a@v1"
            self.assertTrue(pkg_dir.exists(), "Should fall back to source build")
        finally:
            srv.shutdown()
            srv.server_close()

    def test_plain_url_depot_manifest_rejected(self):
        """Plain-URL depot manifest entries are rejected; falls back to source."""
        archives = self._install_and_export(["local.depot_a@v1"])

        srv, port = self._start_server()
        try:
            # Write plain-URL manifest (no SHA256) — should be rejected
            shutil.copy2(archives[0], self.serve_dir / archives[0].name)
            content = f"http://127.0.0.1:{port}/{archives[0].name}\n"
            (self.serve_dir / "depot.txt").write_text(content, encoding="utf-8")
            depot_url = f"http://127.0.0.1:{port}/depot.txt"
            m = self._make_target_manifest(["local.depot_a@v1"], [depot_url])

            r = self._run("sync", "--manifest", str(m), cache_root=self.target_cache)
            self.assertEqual(r.returncode, 0, f"sync failed: {r.stderr}")

            # Should fall back to source build since plain URL was rejected
            pkg_dir = self.target_cache / "packages" / "local.depot_a@v1"
            self.assertTrue(pkg_dir.exists(), "Should fall back to source build")
        finally:
            srv.shutdown()
            srv.server_close()

    def test_sha256_multiple_packages(self):
        """Multiple packages with SHA256 manifest all succeed."""
        archives = self._install_and_export(["local.depot_a@v1", "local.depot_b@v1"])

        srv, port = self._start_server()
        try:
            depot_url = self._make_depot_manifest(archives, port)
            m = self._make_target_manifest(
                ["local.depot_a@v1", "local.depot_b@v1"], [depot_url]
            )

            r = self._run("sync", "--manifest", str(m), cache_root=self.target_cache)
            self.assertEqual(r.returncode, 0, f"sync failed: {r.stderr}")

            for pkg in ["local.depot_a@v1", "local.depot_b@v1"]:
                pkg_dir = self.target_cache / "packages" / pkg
                self.assertTrue(pkg_dir.exists(), f"{pkg} should be cached")
        finally:
            srv.shutdown()
            srv.server_close()

    def test_sha256_corrupt_archive_falls_back(self):
        """Corrupt archive with valid-looking hash -> SHA256 mismatch -> fallback."""
        archives = self._install_and_export(["local.depot_a@v1"])

        srv, port = self._start_server()
        try:
            real_hash = hashlib.sha256(archives[0].read_bytes()).hexdigest()
            corrupt_data = b"this is corrupt data"
            (self.serve_dir / archives[0].name).write_bytes(corrupt_data)

            content = f"{real_hash}  http://127.0.0.1:{port}/{archives[0].name}\n"
            (self.serve_dir / "depot.txt").write_text(content, encoding="utf-8")
            depot_url = f"http://127.0.0.1:{port}/depot.txt"
            m = self._make_target_manifest(["local.depot_a@v1"], [depot_url])

            r = self._run("sync", "--manifest", str(m), cache_root=self.target_cache)
            self.assertEqual(r.returncode, 0, f"sync failed: {r.stderr}")

            pkg_dir = self.target_cache / "packages" / "local.depot_a@v1"
            self.assertTrue(pkg_dir.exists(), "Should fall back to source build")
        finally:
            srv.shutdown()
            srv.server_close()

    def test_depot_manifest_comments_and_blank_lines(self):
        """Depot manifest with comments, blank lines, and SHA256 works."""
        archives = self._install_and_export(["local.depot_a@v1"])

        srv, port = self._start_server()
        try:
            shutil.copy2(archives[0], self.serve_dir / archives[0].name)
            h = hashlib.sha256(archives[0].read_bytes()).hexdigest()
            content = (
                "# Depot manifest for CI\n"
                "\n"
                f"{h}  http://127.0.0.1:{port}/{archives[0].name}\n"
                "\n"
                "# End of manifest\n"
            )
            (self.serve_dir / "depot.txt").write_text(content, encoding="utf-8")
            depot_url = f"http://127.0.0.1:{port}/depot.txt"
            m = self._make_target_manifest(["local.depot_a@v1"], [depot_url])

            r = self._run("sync", "--manifest", str(m), cache_root=self.target_cache)
            self.assertEqual(r.returncode, 0, f"sync failed: {r.stderr}")

            pkg_dir = self.target_cache / "packages" / "local.depot_a@v1"
            self.assertTrue(pkg_dir.exists(), "Should install from depot")
        finally:
            srv.shutdown()
            srv.server_close()

    def test_export_depot_prefix_no_trailing_slash(self):
        """Export with --depot-prefix without trailing slash produces SHA256 format."""
        m = self._make_source_manifest(["local.depot_a@v1"])
        r = self._run("install", "--manifest", str(m))
        self.assertEqual(r.returncode, 0, f"install failed: {r.stderr}")

        r = self._run(
            "export",
            "local.depot_a@v1",
            "-o",
            str(self.output_dir),
            "--depot-prefix",
            "s3://bucket/cache",
            "--manifest",
            str(m),
        )
        self.assertEqual(r.returncode, 0, f"export failed: {r.stderr}")

        lines = [l for l in r.stdout.strip().split("\n") if l.strip()]
        self.assertEqual(len(lines), 1)
        parts = lines[0].split("  ", 1)
        self.assertEqual(len(parts), 2)
        self.assertEqual(len(parts[0]), 64)
        self.assertTrue(parts[1].startswith("s3://bucket/cache"))
        self.assertTrue(parts[1].endswith(".tar.zst"))


class TestIgnoreDepot(EnvyTestCase):
    """Tests for --ignore-depot flag and ENVY_IGNORE_DEPOT env var."""

    def setUp(self):
        self.source_cache = self.make_temp_dir("source_cache")
        self.target_cache = self.make_temp_dir("target_cache")
        self.test_dir = self.make_temp_dir("test_dir")
        self.output_dir = self.make_temp_dir("output_dir")
        self.serve_dir = self.make_temp_dir("serve_dir")
        self.marker_dir = self.make_temp_dir("marker_dir")
        self.envy = test_config.get_envy_executable()
        self.project_root = Path(__file__).parent.parent

        self.archive_path = self.test_dir / "test.tar.gz"
        self.archive_hash = _create_test_archive(self.archive_path)

        # Write specs with build markers for detecting source vs depot
        self.spec_lua = {}
        self.markers = {}
        for name, identity in [
            ("pkg_a.lua", "local.depot_a@v1"),
            ("pkg_b.lua", "local.depot_b@v1"),
        ]:
            marker = self.marker_dir / f"{identity}.marker"
            self.markers[identity] = marker
            (self.test_dir / name).write_text(
                self._make_spec_with_marker(identity, marker),
                encoding="utf-8",
            )
            self.spec_lua[identity] = f"{self.test_dir.as_posix()}/{name}"

    def tearDown(self):
        for d in [
            self.source_cache,
            self.target_cache,
            self.test_dir,
            self.output_dir,
            self.serve_dir,
            self.marker_dir,
        ]:
            shutil.rmtree(d, ignore_errors=True)

    # -- helpers --

    def _run(self, *args, cache_root=None, env_extra=None):
        cache = cache_root or self.target_cache
        cmd = [str(self.envy), "--cache-root", str(cache), *args]
        env = None
        if env_extra:
            env = os.environ.copy()
            env.update(env_extra)
        return test_config.run(
            cmd, cwd=self.project_root, capture_output=True, text=True, env=env
        )

    def _clear_markers(self):
        """Remove all marker files to reset detection state."""
        for m in self.markers.values():
            m.unlink(missing_ok=True)

    def _make_source_manifest(self, identities):
        packages = ", ".join(
            f'{{ spec = "{i}", source = "{self.spec_lua[i]}" }}' for i in identities
        )
        path = self.test_dir / "source.lua"
        path.write_text(
            test_config.make_manifest(f"\nPACKAGES = {{\n    {packages}\n}}\n"),
            encoding="utf-8",
        )
        return path

    def _install_and_export(self, identities):
        m = self._make_source_manifest(identities)
        r = self._run("install", "--manifest", str(m), cache_root=self.source_cache)
        self.assertEqual(r.returncode, 0, f"install failed: {r.stderr}")

        r = self._run(
            "export",
            "-o",
            str(self.output_dir),
            "--manifest",
            str(m),
            cache_root=self.source_cache,
        )
        self.assertEqual(r.returncode, 0, f"export failed: {r.stderr}")
        paths = []
        for line in r.stdout.strip().split("\n"):
            line = line.strip()
            if not line:
                continue
            _, p = parse_export_line(line)
            paths.append(p)
        return paths

    def _start_server(self):
        handler = partial(_QuietHandler, directory=str(self.serve_dir))
        srv = ThreadingHTTPServer(("127.0.0.1", 0), handler)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        return srv, srv.server_address[1]

    def _make_depot_manifest(self, archive_paths, port, name="depot.txt"):
        lines = []
        for ap in archive_paths:
            shutil.copy2(ap, self.serve_dir / ap.name)
            h = hashlib.sha256(ap.read_bytes()).hexdigest()
            lines.append(f"{h}  http://127.0.0.1:{port}/{ap.name}")
        (self.serve_dir / name).write_text("\n".join(lines) + "\n", encoding="utf-8")
        return f"http://127.0.0.1:{port}/{name}"

    def _make_target_manifest(self, identities, depot_urls):
        header = '-- @envy bin "envy-bin"\n'
        packages = ", ".join(
            f'{{ spec = "{i}", source = "{self.spec_lua[i]}" }}' for i in identities
        )
        depots = ", ".join(f'"{u}"' for u in depot_urls)
        path = self.test_dir / "target.lua"
        path.write_text(
            header
            + f"\nPACKAGES = {{\n    {packages}\n}}\n"
            + f"PACKAGE_DEPOTS = {{ {depots} }}\n",
            encoding="utf-8",
        )
        return path

    def _make_spec_with_marker(self, identity, marker_path):
        """Generate a spec whose BUILD writes a marker file to an external path."""
        p = self.archive_path.as_posix()
        mp = marker_path.as_posix()
        return f'''IDENTITY = "{identity}"
EXPORTABLE = true

FETCH = {{
  source = "{p}",
  sha256 = "{self.archive_hash}"
}}

STAGE = {{strip = 1}}

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  if envy.PLATFORM == "windows" then
    envy.run([[Set-Content -Path built.txt -Value "built"]], {{ shell = ENVY_SHELL.POWERSHELL }})
    envy.run([[Set-Content -Path "{mp}" -Value "built"]], {{ shell = ENVY_SHELL.POWERSHELL }})
  else
    envy.run([[echo 'built' > built.txt]])
    envy.run([[echo 'built' > "{mp}"]])
  end
end
'''

    # -- CLI flag tests --

    def test_sync_ignore_depot_rebuilds_from_source(self):
        """sync --ignore-depot with depot configured builds from source."""
        archives = self._install_and_export(["local.depot_a@v1"])
        self._clear_markers()
        srv, port = self._start_server()
        try:
            depot_url = self._make_depot_manifest(archives, port)
            m = self._make_target_manifest(["local.depot_a@v1"], [depot_url])
            r = self._run("sync", "--ignore-depot", "--manifest", str(m))
            self.assertEqual(r.returncode, 0, f"sync failed: {r.stderr}")
            self.assertTrue(
                self.markers["local.depot_a@v1"].exists(),
                "BUILD marker should exist (source build ran)",
            )
        finally:
            srv.shutdown()
            srv.server_close()

    def test_install_ignore_depot_rebuilds_from_source(self):
        """install --ignore-depot with depot configured builds from source."""
        archives = self._install_and_export(["local.depot_a@v1"])
        self._clear_markers()
        srv, port = self._start_server()
        try:
            depot_url = self._make_depot_manifest(archives, port)
            m = self._make_target_manifest(["local.depot_a@v1"], [depot_url])
            r = self._run("install", "--ignore-depot", "--manifest", str(m))
            self.assertEqual(r.returncode, 0, f"install failed: {r.stderr}")
            self.assertTrue(
                self.markers["local.depot_a@v1"].exists(),
                "BUILD marker should exist (source build ran)",
            )
        finally:
            srv.shutdown()
            srv.server_close()

    def test_export_ignore_depot_rebuilds_from_source(self):
        """export --ignore-depot with depot configured builds from source."""
        archives = self._install_and_export(["local.depot_a@v1"])
        self._clear_markers()
        srv, port = self._start_server()
        try:
            depot_url = self._make_depot_manifest(archives, port)
            m = self._make_target_manifest(["local.depot_a@v1"], [depot_url])
            r = self._run(
                "export",
                "--ignore-depot",
                "-o",
                str(self.output_dir),
                "--manifest",
                str(m),
            )
            self.assertEqual(r.returncode, 0, f"export failed: {r.stderr}")
            self.assertTrue(
                self.markers["local.depot_a@v1"].exists(),
                "BUILD marker should exist (source build ran)",
            )
        finally:
            srv.shutdown()
            srv.server_close()

    def test_package_ignore_depot_rebuilds_from_source(self):
        """package --ignore-depot with depot configured builds from source."""
        archives = self._install_and_export(["local.depot_a@v1"])
        self._clear_markers()
        srv, port = self._start_server()
        try:
            depot_url = self._make_depot_manifest(archives, port)
            m = self._make_target_manifest(["local.depot_a@v1"], [depot_url])
            r = self._run(
                "package",
                "--ignore-depot",
                "local.depot_a@v1",
                "--manifest",
                str(m),
            )
            self.assertEqual(r.returncode, 0, f"package failed: {r.stderr}")
            self.assertTrue(
                self.markers["local.depot_a@v1"].exists(),
                "BUILD marker should exist (source build ran)",
            )
        finally:
            srv.shutdown()
            srv.server_close()

    def test_sync_ignore_depot_multiple_packages(self):
        """Both packages rebuild from source with --ignore-depot."""
        archives = self._install_and_export(["local.depot_a@v1", "local.depot_b@v1"])
        self._clear_markers()
        srv, port = self._start_server()
        try:
            depot_url = self._make_depot_manifest(archives, port)
            m = self._make_target_manifest(
                ["local.depot_a@v1", "local.depot_b@v1"],
                [depot_url],
            )
            r = self._run("sync", "--ignore-depot", "--manifest", str(m))
            self.assertEqual(r.returncode, 0, f"sync failed: {r.stderr}")
            for pkg in ["local.depot_a@v1", "local.depot_b@v1"]:
                self.assertTrue(
                    self.markers[pkg].exists(),
                    f"{pkg} BUILD marker should exist",
                )
        finally:
            srv.shutdown()
            srv.server_close()

    def test_sync_without_ignore_depot_uses_depot(self):
        """Control: same manifest without --ignore-depot uses depot (no source build)."""
        archives = self._install_and_export(["local.depot_a@v1"])
        self._clear_markers()
        srv, port = self._start_server()
        try:
            depot_url = self._make_depot_manifest(archives, port)
            m = self._make_target_manifest(["local.depot_a@v1"], [depot_url])
            r = self._run("sync", "--manifest", str(m))
            self.assertEqual(r.returncode, 0, f"sync failed: {r.stderr}")
            self.assertFalse(
                self.markers["local.depot_a@v1"].exists(),
                "BUILD marker should NOT exist (depot used)",
            )
        finally:
            srv.shutdown()
            srv.server_close()

    # -- Env var tests --

    def test_sync_env_var_ignores_depot(self):
        """ENVY_IGNORE_DEPOT=1 in env causes sync to build from source."""
        archives = self._install_and_export(["local.depot_a@v1"])
        self._clear_markers()
        srv, port = self._start_server()
        try:
            depot_url = self._make_depot_manifest(archives, port)
            m = self._make_target_manifest(["local.depot_a@v1"], [depot_url])
            r = self._run(
                "sync",
                "--manifest",
                str(m),
                env_extra={"ENVY_IGNORE_DEPOT": "1"},
            )
            self.assertEqual(r.returncode, 0, f"sync failed: {r.stderr}")
            self.assertTrue(
                self.markers["local.depot_a@v1"].exists(),
                "BUILD marker should exist (env var set)",
            )
        finally:
            srv.shutdown()
            srv.server_close()

    def test_install_env_var_ignores_depot(self):
        """ENVY_IGNORE_DEPOT=1 in env causes install to build from source."""
        archives = self._install_and_export(["local.depot_a@v1"])
        self._clear_markers()
        srv, port = self._start_server()
        try:
            depot_url = self._make_depot_manifest(archives, port)
            m = self._make_target_manifest(["local.depot_a@v1"], [depot_url])
            r = self._run(
                "install",
                "--manifest",
                str(m),
                env_extra={"ENVY_IGNORE_DEPOT": "1"},
            )
            self.assertEqual(r.returncode, 0, f"install failed: {r.stderr}")
            self.assertTrue(
                self.markers["local.depot_a@v1"].exists(),
                "BUILD marker should exist (env var set)",
            )
        finally:
            srv.shutdown()
            srv.server_close()

    def test_export_env_var_ignores_depot(self):
        """ENVY_IGNORE_DEPOT=1 in env causes export to build from source."""
        archives = self._install_and_export(["local.depot_a@v1"])
        self._clear_markers()
        srv, port = self._start_server()
        try:
            depot_url = self._make_depot_manifest(archives, port)
            m = self._make_target_manifest(["local.depot_a@v1"], [depot_url])
            r = self._run(
                "export",
                "-o",
                str(self.output_dir),
                "--manifest",
                str(m),
                env_extra={"ENVY_IGNORE_DEPOT": "1"},
            )
            self.assertEqual(r.returncode, 0, f"export failed: {r.stderr}")
            self.assertTrue(
                self.markers["local.depot_a@v1"].exists(),
                "BUILD marker should exist (env var set)",
            )
        finally:
            srv.shutdown()
            srv.server_close()

    def test_package_env_var_ignores_depot(self):
        """ENVY_IGNORE_DEPOT=1 in env causes package to build from source."""
        archives = self._install_and_export(["local.depot_a@v1"])
        self._clear_markers()
        srv, port = self._start_server()
        try:
            depot_url = self._make_depot_manifest(archives, port)
            m = self._make_target_manifest(["local.depot_a@v1"], [depot_url])
            r = self._run(
                "package",
                "local.depot_a@v1",
                "--manifest",
                str(m),
                env_extra={"ENVY_IGNORE_DEPOT": "1"},
            )
            self.assertEqual(r.returncode, 0, f"package failed: {r.stderr}")
            self.assertTrue(
                self.markers["local.depot_a@v1"].exists(),
                "BUILD marker should exist (env var set)",
            )
        finally:
            srv.shutdown()
            srv.server_close()

    # -- Edge case tests --

    def test_flag_overrides_working_depot(self):
        """Depot has valid archives, --ignore-depot forces source build anyway."""
        archives = self._install_and_export(["local.depot_a@v1"])
        self._clear_markers()
        srv, port = self._start_server()
        try:
            depot_url = self._make_depot_manifest(archives, port)
            m = self._make_target_manifest(["local.depot_a@v1"], [depot_url])

            # First verify depot works without flag
            r = self._run("install", "--manifest", str(m))
            self.assertEqual(r.returncode, 0)
            self.assertFalse(
                self.markers["local.depot_a@v1"].exists(),
                "Without flag: depot should be used (no marker)",
            )

            # Now with flag on a fresh cache
            fresh_cache = self.make_temp_dir("fresh_cache")
            r = self._run(
                "install",
                "--ignore-depot",
                "--manifest",
                str(m),
                cache_root=fresh_cache,
            )
            self.assertEqual(r.returncode, 0, f"install failed: {r.stderr}")
            self.assertTrue(
                self.markers["local.depot_a@v1"].exists(),
                "With flag: BUILD marker should exist",
            )
        finally:
            srv.shutdown()
            srv.server_close()

    def test_env_var_overrides_working_depot(self):
        """Depot has valid archives, ENVY_IGNORE_DEPOT forces source build anyway."""
        archives = self._install_and_export(["local.depot_a@v1"])
        self._clear_markers()
        srv, port = self._start_server()
        try:
            depot_url = self._make_depot_manifest(archives, port)
            m = self._make_target_manifest(["local.depot_a@v1"], [depot_url])

            # First verify depot works without env var
            r = self._run("install", "--manifest", str(m))
            self.assertEqual(r.returncode, 0)
            self.assertFalse(
                self.markers["local.depot_a@v1"].exists(),
                "Without env var: depot should be used (no marker)",
            )

            # Now with env var on a fresh cache
            fresh_cache = self.make_temp_dir("fresh_cache")
            r = self._run(
                "install",
                "--manifest",
                str(m),
                cache_root=fresh_cache,
                env_extra={"ENVY_IGNORE_DEPOT": "1"},
            )
            self.assertEqual(r.returncode, 0, f"install failed: {r.stderr}")
            self.assertTrue(
                self.markers["local.depot_a@v1"].exists(),
                "With env var: BUILD marker should exist",
            )
        finally:
            srv.shutdown()
            srv.server_close()


class TestHashCommand(EnvyTestCase):
    """Tests for 'envy hash' multi-file and --prefix support."""

    def setUp(self):
        self.test_dir = self.make_temp_dir("test_dir")
        self.envy = test_config.get_envy_executable()
        self.project_root = Path(__file__).parent.parent

    def tearDown(self):
        shutil.rmtree(self.test_dir, ignore_errors=True)

    def _run(self, *args):
        cmd = [str(self.envy), *args]
        return test_config.run(
            cmd, cwd=self.project_root, capture_output=True, text=True
        )

    def test_hash_single_file(self):
        """Hash single file produces sha256sum-compatible format."""
        f = self.test_dir / "test.tar.zst"
        f.write_bytes(b"test data")
        expected = hashlib.sha256(b"test data").hexdigest()

        r = self._run("hash", str(f))
        self.assertEqual(r.returncode, 0, f"hash failed: {r.stderr}")

        lines = [l for l in r.stdout.strip().split("\n") if l.strip()]
        self.assertEqual(len(lines), 1)
        parts = lines[0].split("  ", 1)
        self.assertEqual(len(parts), 2)
        self.assertEqual(parts[0], expected)
        self.assertEqual(parts[1], "test.tar.zst")

    def test_hash_with_prefix(self):
        """Hash with --prefix prepends prefix to filename."""
        f = self.test_dir / "test.tar.zst"
        f.write_bytes(b"test data")
        expected = hashlib.sha256(b"test data").hexdigest()

        r = self._run("hash", "--prefix", "s3://bucket/", str(f))
        self.assertEqual(r.returncode, 0, f"hash failed: {r.stderr}")

        lines = [l for l in r.stdout.strip().split("\n") if l.strip()]
        self.assertEqual(len(lines), 1)
        parts = lines[0].split("  ", 1)
        self.assertEqual(parts[0], expected)
        self.assertEqual(parts[1], "s3://bucket/test.tar.zst")

    def test_hash_directory(self):
        """Hash directory processes all .tar.zst files."""
        for name in ["a.tar.zst", "b.tar.zst", "c.txt"]:
            (self.test_dir / name).write_bytes(b"data")

        r = self._run("hash", str(self.test_dir))
        self.assertEqual(r.returncode, 0, f"hash failed: {r.stderr}")

        lines = [l for l in r.stdout.strip().split("\n") if l.strip()]
        # Only .tar.zst files, not .txt
        self.assertEqual(len(lines), 2)
        filenames = [l.split("  ", 1)[1] for l in lines]
        self.assertIn("a.tar.zst", filenames)
        self.assertIn("b.tar.zst", filenames)

    def test_hash_directory_with_prefix(self):
        """Hash directory with --prefix applies to all files."""
        (self.test_dir / "pkg.tar.zst").write_bytes(b"pkg data")

        r = self._run("hash", "--prefix", "https://cdn/", str(self.test_dir))
        self.assertEqual(r.returncode, 0, f"hash failed: {r.stderr}")

        lines = [l for l in r.stdout.strip().split("\n") if l.strip()]
        self.assertEqual(len(lines), 1)
        parts = lines[0].split("  ", 1)
        self.assertEqual(parts[1], "https://cdn/pkg.tar.zst")

    def test_hash_multiple_files(self):
        """Hash multiple individual files."""
        f1 = self.test_dir / "a.tar.zst"
        f2 = self.test_dir / "b.tar.zst"
        f1.write_bytes(b"aaa")
        f2.write_bytes(b"bbb")

        r = self._run("hash", str(f1), str(f2))
        self.assertEqual(r.returncode, 0, f"hash failed: {r.stderr}")

        lines = [l for l in r.stdout.strip().split("\n") if l.strip()]
        self.assertEqual(len(lines), 2)


class TestImportManifest(EnvyTestCase):
    """Tests for 'envy import <manifest.txt>' and 'envy import --dir --checksums'."""

    def setUp(self):
        super().setUp()
        self.source_cache = self.make_temp_dir("source_cache")
        self.test_dir = self.make_temp_dir("test_dir")
        self.output_dir = self.make_temp_dir("output_dir")
        self.serve_dir = self.make_temp_dir("serve_dir")

        self.archive_path = self.test_dir / "test.tar.gz"
        self.archive_hash = _create_test_archive(self.archive_path)

        self.spec_lua = {}
        for name, identity in [
            ("pkg_a.lua", "local.depot_a@v1"),
        ]:
            (self.test_dir / name).write_text(
                _spec_content(identity, self.archive_path, self.archive_hash),
                encoding="utf-8",
            )
            self.spec_lua[identity] = f"{self.test_dir.as_posix()}/{name}"

    def tearDown(self):
        for d in [
            self.cache_root,
            self.source_cache,
            self.test_dir,
            self.output_dir,
            self.serve_dir,
        ]:
            shutil.rmtree(d, ignore_errors=True)

    def _run(self, *args, cache_root=None):
        cache = cache_root or self.cache_root
        cmd = [str(self.envy), "--cache-root", str(cache), *args]
        return test_config.run(
            cmd, cwd=self.project_root, capture_output=True, text=True
        )

    def _install_and_export(self):
        """Install + export local.depot_a@v1. Returns archive paths."""
        packages = f'{{ spec = "local.depot_a@v1", source = "{self.spec_lua["local.depot_a@v1"]}" }}'
        m = self.test_dir / "source.lua"
        m.write_text(
            make_manifest(f"\nPACKAGES = {{\n    {packages}\n}}\n"),
            encoding="utf-8",
        )
        r = self._run("install", "--manifest", str(m), cache_root=self.source_cache)
        self.assertEqual(r.returncode, 0, f"install failed: {r.stderr}")

        r = self._run(
            "export",
            "-o",
            str(self.output_dir),
            "--manifest",
            str(m),
            cache_root=self.source_cache,
        )
        self.assertEqual(r.returncode, 0, f"export failed: {r.stderr}")
        paths = []
        for line in r.stdout.strip().split("\n"):
            line = line.strip()
            if not line:
                continue
            _, p = parse_export_line(line)
            paths.append(p)
        return paths

    def _make_manifest(self):
        """Create envy.lua manifest referencing local.depot_a@v1."""
        packages = f'{{ spec = "local.depot_a@v1", source = "{self.spec_lua["local.depot_a@v1"]}" }}'
        m = self.test_dir / "import.lua"
        m.write_text(
            make_manifest(f"\nPACKAGES = {{\n    {packages}\n}}\n"),
            encoding="utf-8",
        )
        return m

    def test_import_manifest_txt_local_files(self):
        """Import .txt manifest with local file paths goes through engine."""
        archives = self._install_and_export()
        self.assertEqual(len(archives), 1)

        h = hashlib.sha256(archives[0].read_bytes()).hexdigest()
        depot_txt = self.test_dir / "depot.txt"
        depot_txt.write_text(f"{h}  {archives[0]}\n", encoding="utf-8")

        m = self._make_manifest()
        r = self._run("import", str(depot_txt), "--manifest", str(m))
        self.assertEqual(r.returncode, 0, f"import failed: {r.stderr}")

        pkg_dir = self.cache_root / "packages" / "local.depot_a@v1"
        self.assertTrue(pkg_dir.exists(), "Package should be in target cache")

    def test_import_manifest_txt_http(self):
        """Import .txt manifest with HTTP URLs goes through engine."""
        archives = self._install_and_export()
        self.assertEqual(len(archives), 1)

        handler = partial(_QuietHandler, directory=str(self.output_dir))
        srv = ThreadingHTTPServer(("127.0.0.1", 0), handler)
        port = srv.server_address[1]
        threading.Thread(target=srv.serve_forever, daemon=True).start()

        try:
            h = hashlib.sha256(archives[0].read_bytes()).hexdigest()
            depot_txt = self.test_dir / "depot.txt"
            depot_txt.write_text(
                f"{h}  http://127.0.0.1:{port}/{archives[0].name}\n",
                encoding="utf-8",
            )

            m = self._make_manifest()
            r = self._run("import", str(depot_txt), "--manifest", str(m))
            self.assertEqual(r.returncode, 0, f"import failed: {r.stderr}")

            pkg_dir = self.cache_root / "packages" / "local.depot_a@v1"
            self.assertTrue(pkg_dir.exists(), "Package should be in target cache")
        finally:
            srv.shutdown()
            srv.server_close()

    def test_depot_download_row_names_the_url(self):
        """The depot progress row must identify the archive being downloaded.

        Every depot archive is written to the same fixed temp filename, so labeling
        the row with the destination made every package's row read
        "depot-archive.tar.zst". The row is labeled with the source URL instead.

        Served in throttled chunks so the fallback renderer (throttle forced to 0)
        gets at least one cycle mid-transfer; without that the transfer can finish
        before any row is drawn.
        """
        archives = self._install_and_export()
        self.assertEqual(len(archives), 1)

        archive_bytes = archives[0].read_bytes()
        archive_name = archives[0].name

        class _SlowHandler(BaseHTTPRequestHandler):
            def log_message(self, fmt, *args):
                return

            def do_GET(self):  # noqa: N802 - BaseHTTPRequestHandler API
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(len(archive_bytes)))
                self.end_headers()
                chunk = max(1, len(archive_bytes) // 8)
                for start in range(0, len(archive_bytes), chunk):
                    self.wfile.write(archive_bytes[start : start + chunk])
                    self.wfile.flush()
                    time.sleep(0.25)

        srv = ThreadingHTTPServer(("127.0.0.1", 0), _SlowHandler)
        port = srv.server_address[1]
        threading.Thread(target=srv.serve_forever, daemon=True).start()

        try:
            url = f"http://127.0.0.1:{port}/{archive_name}"
            depot_txt = self.test_dir / "depot.txt"
            depot_txt.write_text(
                f"{hashlib.sha256(archive_bytes).hexdigest()}  {url}\n", encoding="utf-8"
            )

            m = self._make_manifest()
            env = test_config.get_test_env()
            env["TERM"] = "dumb"
            env["ENVY_TEST_FALLBACK_THROTTLE_MS"] = "0"
            r = test_config.run(
                [
                    str(self.envy),
                    "--cache-root",
                    str(self.cache_root),
                    "import",
                    str(depot_txt),
                    "--manifest",
                    str(m),
                ],
                cwd=self.project_root,
                capture_output=True,
                text=True,
                env=env,
            )
            self.assertEqual(r.returncode, 0, f"import failed: {r.stderr}")

            progress_rows = [
                ln
                for ln in r.stderr.splitlines()
                if "local.depot_a@v1" in ln and "MB" not in ln and "B" in ln
            ]
            self.assertTrue(progress_rows, f"no progress row rendered: {r.stderr}")
            self.assertNotIn("depot-archive.tar.zst", r.stderr)
            self.assertTrue(
                any(url in ln for ln in progress_rows),
                f"progress rows do not name the URL: {progress_rows}",
            )
        finally:
            srv.shutdown()
            srv.server_close()

    def test_import_dir_with_checksums(self):
        """Import --dir --checksums populates SHA256 from checksums file."""
        archives = self._install_and_export()
        self.assertEqual(len(archives), 1)

        # Copy archive to a serve dir
        import_dir = self.make_temp_dir("import_dir")
        shutil.copy2(archives[0], import_dir / archives[0].name)

        h = hashlib.sha256(archives[0].read_bytes()).hexdigest()
        checksums = self.test_dir / "checksums.txt"
        checksums.write_text(f"{h}  {archives[0].name}\n", encoding="utf-8")

        packages = f'{{ spec = "local.depot_a@v1", source = "{self.spec_lua["local.depot_a@v1"]}" }}'
        m = self.test_dir / "import.lua"
        m.write_text(
            make_manifest(f"\nPACKAGES = {{\n    {packages}\n}}\n"),
            encoding="utf-8",
        )

        r = self._run(
            "import",
            "--dir",
            str(import_dir),
            "--checksums",
            str(checksums),
            "--manifest",
            str(m),
        )
        self.assertEqual(r.returncode, 0, f"import failed: {r.stderr}")
    def test_import_dir_without_checksums(self):
        """Import --dir without --checksums works (no SHA256 required)."""
        archives = self._install_and_export()
        self.assertEqual(len(archives), 1)

        import_dir = self.make_temp_dir("import_dir")
        shutil.copy2(archives[0], import_dir / archives[0].name)

        m = self._make_manifest()
        r = self._run(
            "import",
            "--dir",
            str(import_dir),
            "--manifest",
            str(m),
        )
        self.assertEqual(r.returncode, 0, f"import failed: {r.stderr}")

        pkg_dir = self.cache_root / "packages" / "local.depot_a@v1"
        self.assertTrue(pkg_dir.exists(), "Package should be in target cache")
    def test_import_manifest_txt_plain_url_rejected(self):
        """Import .txt manifest with plain URLs (no SHA256) falls back to source."""
        archives = self._install_and_export()
        self.assertEqual(len(archives), 1)

        # Write plain-URL manifest (no SHA256 prefix)
        depot_txt = self.test_dir / "depot.txt"
        depot_txt.write_text(f"{archives[0]}\n", encoding="utf-8")

        m = self._make_manifest()
        r = self._run("import", str(depot_txt), "--manifest", str(m))
        # Should succeed (falls back to source build) but depot entry is rejected
        self.assertEqual(r.returncode, 0, f"import failed: {r.stderr}")

    def test_import_dir_wrong_checksums_falls_back(self):
        """Import --dir --checksums with wrong SHA256 falls back to source."""
        archives = self._install_and_export()
        self.assertEqual(len(archives), 1)

        import_dir = self.make_temp_dir("import_dir")
        shutil.copy2(archives[0], import_dir / archives[0].name)

        wrong_hash = "0" * 64
        checksums = self.test_dir / "checksums.txt"
        checksums.write_text(
            f"{wrong_hash}  {archives[0].name}\n", encoding="utf-8"
        )

        m = self._make_manifest()
        r = self._run(
            "import",
            "--dir",
            str(import_dir),
            "--checksums",
            str(checksums),
            "--manifest",
            str(m),
        )
        # Should succeed — falls back to source build after SHA256 mismatch
        self.assertEqual(r.returncode, 0, f"import failed: {r.stderr}")

        pkg_dir = self.cache_root / "packages" / "local.depot_a@v1"
        self.assertTrue(pkg_dir.exists(), "Should fall back to source build")
def _make_tracking_handler(request_log, directory):
    """Create a handler class with its own isolated request log."""

    class _Handler(SimpleHTTPRequestHandler):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, directory=directory, **kwargs)

        def log_message(self, format, *args):
            return

        def do_GET(self):
            request_log.append(self.path)
            super().do_GET()

    return _Handler


class TestUserManagedSkipsDepot(EnvyTestCase):
    """User-managed-only manifests must not fetch the package-depot manifest."""

    def setUp(self):
        super().setUp()
        self.test_dir = self.make_temp_dir("test_dir")
        self.serve_dir = self.make_temp_dir("serve_dir")
        self.marker_dir = self.make_temp_dir("marker_dir")
        self.requests = []

    def tearDown(self):
        for d in [self.cache_root, self.test_dir, self.serve_dir, self.marker_dir]:
            shutil.rmtree(d, ignore_errors=True)

    def _run(self, *args, env_extra=None):
        cmd = [str(self.envy), "--cache-root", str(self.cache_root), *args]
        env = os.environ.copy()
        if env_extra:
            env.update(env_extra)
        return test_config.run(
            cmd, cwd=self.project_root, capture_output=True, text=True, env=env
        )

    def _start_tracking_server(self):
        handler = _make_tracking_handler(self.requests, str(self.serve_dir))
        srv = ThreadingHTTPServer(("127.0.0.1", 0), handler)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        return srv, srv.server_address[1]

    def _write_user_managed_spec(self, name, identity, marker_path):
        mp = marker_path.as_posix()
        content = f'''IDENTITY = "{identity}"

USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options)
        local f = io.open("{mp}", "r")
        if f then f:close(); return true end
        return false
    end,
    INSTALL = function(pkg_dir, options)
        local f = io.open("{mp}", "w")
        if not f then error("Failed to create marker") end
        f:write("installed")
        f:close()
    end,
  }},
}}

'''
        path = self.test_dir / f"{name}.lua"
        path.write_text(content, encoding="utf-8")
        return path

    def _make_manifest_with_depot(self, specs, depot_url):
        """specs: (identity, path) or (identity, path, setup_pair_names) tuples."""
        header = '-- @envy bin "envy-bin"\n'

        def entry(spec):
            ident, path = spec[0], spec[1]
            setup = (
                ", setup = { " + ", ".join(f'"{n}"' for n in spec[2]) + " }"
                if len(spec) > 2
                else ""
            )
            return f'{{ spec = "{ident}", source = "{path.as_posix()}"{setup} }}'

        packages = ", ".join(entry(s) for s in specs)
        manifest = self.test_dir / "envy.lua"
        manifest.write_text(
            header
            + f"\nPACKAGES = {{\n    {packages}\n}}\n"
            + f'PACKAGE_DEPOTS = {{ "{depot_url}" }}\n',
            encoding="utf-8",
        )
        return manifest

    def test_only_user_managed_does_not_fetch_depot(self):
        """Sync with only user-managed packages never contacts depot server."""
        marker = self.marker_dir / "marker.txt"
        spec = self._write_user_managed_spec("um_pkg", "local.um_depot_test@v1", marker)
        (self.serve_dir / "depot.txt").write_text("", encoding="utf-8")

        srv, port = self._start_tracking_server()
        try:
            depot_url = f"http://127.0.0.1:{port}/depot.txt"
            m = self._make_manifest_with_depot(
                [("local.um_depot_test@v1", spec, ("main",))], depot_url
            )
            r = self._run("sync", "--manifest", str(m))
            self.assertEqual(r.returncode, 0, f"sync failed: {r.stderr}")
            self.assertTrue(marker.exists(), "Install should have run")
            self.assertEqual(
                self.requests,
                [],
                f"Depot manifest should not be fetched for user-managed-only "
                f"packages, but got requests: {self.requests}",
            )
        finally:
            srv.shutdown()
            srv.server_close()

    def test_mixed_user_and_cache_managed_fetches_depot(self):
        """Sync with both user-managed and cache-managed packages does fetch depot."""
        marker = self.marker_dir / "marker.txt"
        um_spec = self._write_user_managed_spec(
            "um_pkg", "local.um_mixed_test@v1", marker
        )

        archive_path = self.test_dir / "test.tar.gz"
        _create_test_archive(archive_path)
        archive_hash = hashlib.sha256(archive_path.read_bytes()).hexdigest()
        cm_spec = self.test_dir / "cm_pkg.lua"
        cm_spec.write_text(
            _spec_content("local.cm_mixed_test@v1", archive_path, archive_hash),
            encoding="utf-8",
        )

        (self.serve_dir / "depot.txt").write_text("", encoding="utf-8")

        srv, port = self._start_tracking_server()
        try:
            depot_url = f"http://127.0.0.1:{port}/depot.txt"
            m = self._make_manifest_with_depot(
                [
                    ("local.um_mixed_test@v1", um_spec),
                    ("local.cm_mixed_test@v1", cm_spec),
                ],
                depot_url,
            )
            r = self._run("sync", "--manifest", str(m))
            self.assertEqual(r.returncode, 0, f"sync failed: {r.stderr}")

            depot_fetches = [rq for rq in self.requests if "depot.txt" in rq]
            self.assertGreater(
                len(depot_fetches),
                0,
                "Depot manifest should be fetched when cache-managed packages exist",
            )
        finally:
            srv.shutdown()
            srv.server_close()


class TestDepotFetchFunction(EnvyTestCase):
    """PACKAGE_DEPOTS FETCH-function entries: text/path/entries returns, DEPENDS
    on a manifest tool package, failure fatality, --ignore-depot laziness."""

    def setUp(self):
        self.source_cache = self.make_temp_dir("source_cache")
        self.target_cache = self.make_temp_dir("target_cache")
        self.test_dir = self.make_temp_dir("test_dir")
        self.output_dir = self.make_temp_dir("output_dir")
        self.serve_dir = self.make_temp_dir("serve_dir")
        self.marker_dir = self.make_temp_dir("marker_dir")
        self.envy = test_config.get_envy_executable()
        self.project_root = Path(__file__).parent.parent

        self.archive_path = self.test_dir / "test.tar.gz"
        self.archive_hash = _create_test_archive(self.archive_path)

        # Downstream package with an external build marker (source build detector)
        self.pkg_identity = "local.dfn_pkg@v1"
        self.pkg_marker = self.marker_dir / "pkg.marker"
        pkg_spec = self.test_dir / "dfn_pkg.lua"
        pkg_spec.write_text(
            self._spec_with_marker(self.pkg_identity, self.pkg_marker),
            encoding="utf-8",
        )
        self.pkg_spec = pkg_spec

    def tearDown(self):
        for d in [
            self.source_cache,
            self.target_cache,
            self.test_dir,
            self.output_dir,
            self.serve_dir,
            self.marker_dir,
        ]:
            shutil.rmtree(d, ignore_errors=True)

    # -- helpers --

    def _spec_with_marker(self, identity, marker_path, build_body=""):
        p = self.archive_path.as_posix()
        mp = marker_path.as_posix()
        return f'''IDENTITY = "{identity}"
EXPORTABLE = true

FETCH = {{
  source = "{p}",
  sha256 = "{self.archive_hash}"
}}

STAGE = {{strip = 1}}

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  local m = io.open("{mp}", "w"); m:write("built"); m:close()
{build_body}
end
'''

    def _run(self, *args, cache_root=None):
        cache = cache_root or self.target_cache
        cmd = [str(self.envy), "--cache-root", str(cache), *args]
        return test_config.run(
            cmd, cwd=self.project_root, capture_output=True, text=True
        )

    def _install_and_export_pkg(self):
        """Install + export the downstream package. Returns (archive, sha256)."""
        m = self.test_dir / "source.lua"
        m.write_text(
            make_manifest(
                f'\nPACKAGES = {{\n    {{ spec = "{self.pkg_identity}", '
                f'source = "{self.pkg_spec.as_posix()}" }}\n}}\n'
            ),
            encoding="utf-8",
        )
        r = self._run("install", "--manifest", str(m), cache_root=self.source_cache)
        self.assertEqual(r.returncode, 0, f"install failed: {r.stderr}")
        r = self._run(
            "export",
            "-o",
            str(self.output_dir),
            "--manifest",
            str(m),
            cache_root=self.source_cache,
        )
        self.assertEqual(r.returncode, 0, f"export failed: {r.stderr}")
        lines = [l for l in r.stdout.strip().split("\n") if l.strip()]
        self.assertEqual(len(lines), 1)
        _, archive = parse_export_line(lines[0])
        self.pkg_marker.unlink(missing_ok=True)
        return archive, hashlib.sha256(archive.read_bytes()).hexdigest()

    def _serve(self, archive):
        """Serve archive over HTTP. Returns (server, archive_url, depot_line)."""
        shutil.copy2(archive, self.serve_dir / archive.name)
        handler = partial(_QuietHandler, directory=str(self.serve_dir))
        srv = ThreadingHTTPServer(("127.0.0.1", 0), handler)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        port = srv.server_address[1]
        url = f"http://127.0.0.1:{port}/{archive.name}"
        sha = hashlib.sha256(archive.read_bytes()).hexdigest()
        return srv, url, f"{sha}  {url}"

    def _write_manifest(self, depots_lua, extra_packages=""):
        path = self.test_dir / "target.lua"
        path.write_text(
            '-- @envy bin "envy-bin"\n'
            + f"\nPACKAGES = {{\n"
            + f'    {{ spec = "{self.pkg_identity}", source = "{self.pkg_spec.as_posix()}" }}{extra_packages}\n'
            + "}\n"
            + f"PACKAGE_DEPOTS = {depots_lua}\n",
            encoding="utf-8",
        )
        return path

    def _assert_imported(self, r):
        self.assertEqual(r.returncode, 0, f"sync failed: {r.stderr}")
        pkg_dir = self.target_cache / "packages" / self.pkg_identity
        self.assertTrue(pkg_dir.exists(), "Package should be in target cache")
        self.assertFalse(
            self.pkg_marker.exists(),
            "BUILD marker should NOT exist (depot import expected)",
        )

    # -- tests --

    def test_fetch_returns_path(self):
        """FETCH returning a path to a depot text file imports the package."""
        archive, _ = self._install_and_export_pkg()
        srv, _, depot_line = self._serve(archive)
        try:
            depot_txt = self.test_dir / "fn_depot.txt"
            depot_txt.write_text(depot_line + "\n", encoding="utf-8")
            m = self._write_manifest(
                f'{{ {{ FETCH = function(ctx) return "{depot_txt.as_posix()}" end }} }}'
            )
            self._assert_imported(self._run("sync", "--manifest", str(m)))
        finally:
            srv.shutdown()
            srv.server_close()

    def test_fetch_returns_raw_text(self):
        """FETCH returning depot manifest text imports the package."""
        archive, _ = self._install_and_export_pkg()
        srv, _, depot_line = self._serve(archive)
        try:
            m = self._write_manifest(
                f'{{ {{ FETCH = function(ctx) return "{depot_line}\\n" end }} }}'
            )
            self._assert_imported(self._run("sync", "--manifest", str(m)))
        finally:
            srv.shutdown()
            srv.server_close()

    def test_fetch_returns_entries_table(self):
        """FETCH returning an entries table imports the package."""
        archive, sha = self._install_and_export_pkg()
        srv, url, _ = self._serve(archive)
        try:
            m = self._write_manifest(
                "{ { FETCH = function(ctx)\n"
                f'      return {{ {{ url = "{url}", sha256 = "{sha}" }} }}\n'
                "    end } }"
            )
            self._assert_imported(self._run("sync", "--manifest", str(m)))
        finally:
            srv.shutdown()
            srv.server_close()

    def test_fetch_with_depends_tool_builds_from_source(self):
        """DEPENDS tool source-builds (never imports); downstream imports."""
        archive, _ = self._install_and_export_pkg()
        srv, _, depot_line = self._serve(archive)
        try:
            tool_identity = "local.dfn_tool@v1"
            tool_marker = self.marker_dir / "tool.marker"
            tool_spec = self.test_dir / "dfn_tool.lua"
            tool_spec.write_text(
                self._spec_with_marker(
                    tool_identity,
                    tool_marker,
                    build_body=(
                        f'  local f = io.open(install_dir .. "/depot.txt", "w")\n'
                        f'  f:write("{depot_line}\\n"); f:close()\n'
                    ),
                ),
                encoding="utf-8",
            )

            m = self._write_manifest(
                "{ {\n"
                f'    DEPENDS = {{ "{tool_identity}" }},\n'
                "    FETCH = function(ctx)\n"
                f'      local f = io.open(ctx.deps["{tool_identity}"].pkg_path .. "/depot.txt", "r")\n'
                '      local text = f:read("*a"); f:close()\n'
                "      return text\n"
                "    end,\n"
                "  } }",
                extra_packages=(
                    f',\n    {{ spec = "{tool_identity}", '
                    f'source = "{tool_spec.as_posix()}" }}'
                ),
            )

            r = self._run("sync", "--manifest", str(m))
            self.assertEqual(r.returncode, 0, f"sync failed: {r.stderr}")
            self.assertTrue(
                tool_marker.exists(),
                "Tool BUILD marker should exist (source-built, never imports)",
            )
            self.assertFalse(
                self.pkg_marker.exists(),
                "Downstream BUILD marker should NOT exist (depot import)",
            )
        finally:
            srv.shutdown()
            srv.server_close()

    def test_depot_dependency_failure_is_fatal(self):
        """A failing depot DEPENDS build fails the run."""
        tool_identity = "local.dfn_badtool@v1"
        tool_spec = self.test_dir / "dfn_badtool.lua"
        tool_spec.write_text(
            f'''IDENTITY = "{tool_identity}"
FETCH = {{ source = "{self.archive_path.as_posix()}", sha256 = "{self.archive_hash}" }}
STAGE = {{strip = 1}}
BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  error("tool build exploded")
end
''',
            encoding="utf-8",
        )
        m = self._write_manifest(
            "{ {\n"
            f'    DEPENDS = {{ "{tool_identity}" }},\n'
            '    FETCH = function(ctx) return "" end,\n'
            "  } }",
            extra_packages=(
                f',\n    {{ spec = "{tool_identity}", '
                f'source = "{tool_spec.as_posix()}" }}'
            ),
        )
        r = self._run("sync", "--manifest", str(m))
        self.assertNotEqual(r.returncode, 0, "sync should fail when depot dep fails")
        self.assertIn("tool build exploded", r.stderr + r.stdout)

    def test_fetch_error_is_fatal(self):
        """A throwing FETCH function fails the run."""
        m = self._write_manifest(
            '{ { FETCH = function(ctx) error("depot fetch exploded") end } }'
        )
        r = self._run("sync", "--manifest", str(m))
        self.assertNotEqual(r.returncode, 0, "sync should fail when FETCH throws")
        self.assertIn("depot fetch exploded", r.stderr + r.stdout)

    def test_unreachable_uri_degrades_gracefully(self):
        """A dead URI entry warns and skips; other entries still serve imports."""
        archive, _ = self._install_and_export_pkg()
        srv, _, depot_line = self._serve(archive)
        try:
            depot_txt = self.serve_dir / "depot.txt"
            depot_txt.write_text(depot_line + "\n", encoding="utf-8")
            port = srv.server_address[1]
            dead_port = _get_free_port()
            m = self._write_manifest(
                f'{{ "http://127.0.0.1:{dead_port}/depot.txt", '
                f'"http://127.0.0.1:{port}/depot.txt" }}'
            )
            self._assert_imported(self._run("sync", "--manifest", str(m)))
        finally:
            srv.shutdown()
            srv.server_close()

    def test_ignore_depot_skips_fetch_fn_and_depends(self):
        """--ignore-depot: no depot task, tool never built, source build runs."""
        tool_identity = "local.dfn_lazytool@v1"
        tool_marker = self.marker_dir / "lazytool.marker"
        tool_spec = self.test_dir / "dfn_lazytool.lua"
        tool_spec.write_text(
            self._spec_with_marker(tool_identity, tool_marker),
            encoding="utf-8",
        )
        m = self._write_manifest(
            "{ {\n"
            f'    DEPENDS = {{ "{tool_identity}" }},\n'
            '    FETCH = function(ctx) error("must not run") end,\n'
            "  } }"
        )
        r = self._run("sync", "--ignore-depot", "--manifest", str(m))
        self.assertEqual(r.returncode, 0, f"sync failed: {r.stderr}")
        self.assertTrue(
            self.pkg_marker.exists(),
            "Downstream BUILD marker should exist (source build)",
        )
        self.assertFalse(
            tool_marker.exists(),
            "Tool should never be built when depot is ignored",
        )

    def test_duplicate_key_differing_sha_keeps_first(self):
        """Duplicate stem across depots with differing SHA256: first wins."""
        archive, _ = self._install_and_export_pkg()
        srv, url, depot_line = self._serve(archive)
        try:
            port = srv.server_address[1]
            (self.serve_dir / "good.txt").write_text(
                depot_line + "\n", encoding="utf-8"
            )
            (self.serve_dir / "bad.txt").write_text(
                f"{'0' * 64}  {url}\n", encoding="utf-8"
            )
            m = self._write_manifest(
                f'{{ "http://127.0.0.1:{port}/good.txt", '
                f'"http://127.0.0.1:{port}/bad.txt" }}'
            )
            self._assert_imported(self._run("sync", "--manifest", str(m)))
        finally:
            srv.shutdown()
            srv.server_close()

    def test_old_package_depot_directive_is_hard_error(self):
        """The removed @envy package-depot directive fails with migration hint."""
        m = self.test_dir / "old.lua"
        m.write_text(
            '-- @envy bin "envy-bin"\n'
            '-- @envy package-depot "https://example.com/depot.txt"\n'
            "PACKAGES = {}\n",
            encoding="utf-8",
        )
        r = self._run("sync", "--manifest", str(m))
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("PACKAGE_DEPOTS", r.stderr + r.stdout)


if __name__ == "__main__":
    unittest.main()
