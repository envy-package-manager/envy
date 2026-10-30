"""Functional tests for git repository fetching.

Tests git repository support with ref specification (tags, branches, SHAs),
error handling, and basic integration scenarios.
"""

import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
import unittest

from . import test_config
from .env import EnvyTestCase


def _is_macos_ci_sanitizer_build() -> bool:
    """Check if running on macOS CI with sanitizers (where timing issues occur)."""
    if sys.platform != "darwin":
        return False
    if not os.environ.get("GITHUB_ACTIONS"):
        return False
    # CI sanitizer shards have "func-asan" or "func-tsan" in their names
    # and the test runner sets suppression env vars
    return bool(os.environ.get("TSAN_OPTIONS") or os.environ.get("ASAN_OPTIONS"))


class TestFetchGit(EnvyTestCase):
    """Tests for git repository fetching via fetch command."""

    # Real clones from github.com/googlesource.com; network latency exceeds the
    # default per-test watchdog. Relaxed budget still catches true hangs.
    envy_watchdog_timeout = 60

    def setUp(self):
        super().setUp()
        self.specs_dir = self.make_temp_dir("specs_dir")
        self.trace_flag = ["--trace"] if os.environ.get("ENVY_TEST_TRACE") else []

    # ========================================================================
    # Basic Success Cases
    # ========================================================================

    def test_clone_public_https_tag(self):
        """Clone a public HTTPS repository with a valid tag."""
        with tempfile.TemporaryDirectory() as temp_dir:
            dest = Path(temp_dir) / "ninja"

            result = test_config.run(
                [
                    str(self.envy),
                    "fetch",
                    "https://github.com/ninja-build/ninja.git",
                    str(dest),
                    "--ref",
                    "v1.13.2",
                ],
                capture_output=True,
                text=True,
                env={**os.environ, "ENVY_CACHE_DIR": str(self.cache_root)},
            )

            self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
            self.assertTrue(dest.exists(), f"Destination {dest} should exist")
            self.assertTrue((dest / "README.md").exists(), "README.md should exist")
            self.assertTrue((dest / "src").is_dir(), "src directory should exist")

            # Verify .git directory is present (kept for packages that need it)
            self.assertTrue(
                (dest / ".git").exists(), ".git directory should be present"
            )

    def test_clone_public_https_branch(self):
        """Clone a public HTTPS repository with a branch ref."""
        with tempfile.TemporaryDirectory() as temp_dir:
            dest = Path(temp_dir) / "ninja"

            result = test_config.run(
                [
                    str(self.envy),
                    "fetch",
                    "https://github.com/ninja-build/ninja.git",
                    str(dest),
                    "--ref",
                    "master",  # ninja's default branch
                ],
                capture_output=True,
                text=True,
                env={**os.environ, "ENVY_CACHE_DIR": str(self.cache_root)},
            )

            self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
            self.assertTrue(dest.exists())
            self.assertTrue(
                (dest / ".git").exists(), ".git directory should be present"
            )

    @unittest.skipIf(
        _is_macos_ci_sanitizer_build(),
        "SecureTransport has timing issues with googlesource.com under CI sanitizers",
    )
    def test_clone_googlesource_shallow_fallback(self):
        """Clone from googlesource.com which requires full clone fallback.

        googlesource.com servers have compatibility issues with libgit2's
        shallow clone. This test verifies the automatic fallback to full clone.
        """
        with tempfile.TemporaryDirectory() as temp_dir:
            dest = Path(temp_dir) / "gn"

            result = test_config.run(
                [
                    str(self.envy),
                    "fetch",
                    "https://gn.googlesource.com/gn.git",
                    str(dest),
                    "--ref",
                    "main",
                ],
                capture_output=True,
                text=True,
                env={**os.environ, "ENVY_CACHE_DIR": str(self.cache_root)},
            )

            self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
            self.assertTrue(dest.exists(), f"Destination {dest} should exist")
            self.assertTrue((dest / "README.md").exists(), "README.md should exist")
            self.assertTrue((dest / "src").is_dir(), "src directory should exist")
            self.assertTrue(
                (dest / ".git").exists(), ".git directory should be present"
            )

    def test_verify_file_contents_correct(self):
        """Verify that cloned repository has correct file contents."""
        with tempfile.TemporaryDirectory() as temp_dir:
            dest = Path(temp_dir) / "ninja"

            result = test_config.run(
                [
                    str(self.envy),
                    "fetch",
                    "https://github.com/ninja-build/ninja.git",
                    str(dest),
                    "--ref",
                    "v1.13.2",
                ],
                capture_output=True,
                text=True,
                env={**os.environ, "ENVY_CACHE_DIR": str(self.cache_root)},
            )

            self.assertEqual(result.returncode, 0)

            # Check specific file content
            readme = dest / "README.md"
            self.assertTrue(readme.exists())
            content = readme.read_text()
            self.assertIn("Ninja", content)

            # Check directory structure
            self.assertTrue((dest / "src" / "ninja.cc").exists())
            self.assertTrue((dest / "configure.py").exists())

    # ========================================================================
    # Error Cases
    # ========================================================================

    def _serve(self, on_connect) -> int:
        """A localhost TCP server handing each connection to on_connect; returns its port."""
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0))
        listener.listen()
        held: list[socket.socket] = []

        def accept():
            while True:
                try:
                    held.append(listener.accept()[0])
                except OSError:
                    return
                on_connect(held[-1])

        threading.Thread(target=accept, daemon=True).start()

        def close():
            listener.close()
            for conn in held:
                conn.close()

        self.addCleanup(close)
        return listener.getsockname()[1]

    def _retry_reasons(self, url: str, git_timeout_ms: int = 1000) -> list[str]:
        """Fetch url, failing, with one retry allowed: why envy retried."""
        try:
            run = self.run_envy(
                "fetch", url, self.work / "out", "--ref", "main",
                env={**os.environ, "ENVY_GIT_TIMEOUT_MS": str(git_timeout_ms),
                     "ENVY_FETCH_ATTEMPTS": "2", "ENVY_FETCH_RETRY_BASE_MS": "1"},
                timeout=30,
            )
        except subprocess.TimeoutExpired:
            self.fail(f"envy hung on {url}")
        self.assertNotEqual(0, run.returncode)
        return [e.raw["reason"] for e in run.events("download_retry")]

    @staticmethod
    def _reset(conn: socket.socket) -> None:
        """Read the TLS ClientHello, then reset. The pause lets the client block in its
        read first: libgit2 rereads after any error, and a reread after a reset is end of
        stream, so a reset that beat the first read would arrive as EOF."""
        try:
            conn.recv(65536)
            time.sleep(0.5)
            conn.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
            conn.close()
        except OSError:
            pass

    def test_stalled_server_is_retried_as_a_timeout(self):
        """A server that accepts and then says nothing must not hold a fetch forever:
        libgit2's own default is no timeout at all."""
        port = self._serve(lambda conn: None)
        self.assertEqual(["timeout"], self._retry_reasons(f"git://127.0.0.1:{port}/x.git"))

    def test_stalled_https_server_is_retried_as_a_timeout(self):
        """The same over https, through libgit2_https_transport_patch: WinHTTP (Windows)
        ignored the timeout, and worded it in the UI language when told; mbedTLS
        (elsewhere) relabeled it an SSL error, which is fatal. The server never answers
        the TLS handshake, so no certificate is needed."""
        port = self._serve(lambda conn: None)
        self.assertEqual(["timeout"], self._retry_reasons(f"https://127.0.0.1:{port}/x.git"))

    @unittest.skipIf(sys.platform == "win32",
                     "WinHTTP raises every failure but a timeout as GIT_ERROR_OS: fatal")
    def test_https_reset_is_retried_as_a_transfer_failure(self):
        """mbedTLS relabeled the socket stream's failures SSL errors, so a mirror that
        reset an https fetch was never retried."""
        port = self._serve(self._reset)
        self.assertEqual(["transfer"], self._retry_reasons(f"https://127.0.0.1:{port}/x.git",
                                                           git_timeout_ms=10000))

    def test_nonexistent_repository(self):
        """Non-existent repository URL should fail."""
        with tempfile.TemporaryDirectory() as temp_dir:
            dest = Path(temp_dir) / "output"

            result = test_config.run(
                [
                    str(self.envy),
                    "fetch",
                    "https://github.com/this-org-does-not-exist-12345/repo.git",
                    str(dest),
                    "--ref",
                    "main",
                ],
                capture_output=True,
                text=True,
                env={**os.environ, "ENVY_CACHE_DIR": str(self.cache_root)},
            )

            self.assertNotEqual(result.returncode, 0)
            stderr_lower = result.stderr.lower()
            self.assertTrue(
                "clone failed" in stderr_lower or "failed" in stderr_lower,
                f"Expected clone error, got: {result.stderr}",
            )

    def test_valid_repo_nonexistent_ref(self):
        """Valid repository but non-existent ref should fail."""
        with tempfile.TemporaryDirectory() as temp_dir:
            dest = Path(temp_dir) / "ninja"

            result = test_config.run(
                [
                    str(self.envy),
                    "fetch",
                    "https://github.com/ninja-build/ninja.git",
                    str(dest),
                    "--ref",
                    "this-ref-does-not-exist-12345",
                ],
                capture_output=True,
                text=True,
                env={**os.environ, "ENVY_CACHE_DIR": str(self.cache_root)},
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertGreater(len(result.stderr), 0)

    def test_missing_ref_flag(self):
        """Git URL without --ref flag should fail."""
        with tempfile.TemporaryDirectory() as temp_dir:
            dest = Path(temp_dir) / "output"

            result = test_config.run(
                [
                    str(self.envy),
                    "fetch",
                    "https://github.com/ninja-build/ninja.git",
                    str(dest),
                ],
                capture_output=True,
                text=True,
                env={**os.environ, "ENVY_CACHE_DIR": str(self.cache_root)},
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("ref", result.stderr.lower())

    # ========================================================================
    # Edge Cases
    # ========================================================================

    def test_clone_to_path_with_spaces(self):
        """Clone to a destination path containing spaces."""
        with tempfile.TemporaryDirectory() as temp_dir:
            dest = Path(temp_dir) / "path with spaces" / "ninja build"

            result = test_config.run(
                [
                    str(self.envy),
                    "fetch",
                    "https://github.com/ninja-build/ninja.git",
                    str(dest),
                    "--ref",
                    "v1.13.2",
                ],
                capture_output=True,
                text=True,
                env={**os.environ, "ENVY_CACHE_DIR": str(self.cache_root)},
            )

            self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
            self.assertTrue(dest.exists())
            self.assertTrue((dest / "README.md").exists())

    # ========================================================================
    # Integration with Spec System
    # ========================================================================

    def test_spec_with_git_source(self):
        """Spec manifest with git source + ref loads correctly."""
        spec_content = """-- Test spec with git source
IDENTITY = "test.ninja@v1"

FETCH = { source = "https://github.com/ninja-build/ninja.git", ref = "v1.13.2" }

function INSTALL(install_dir, stage_dir, fetch_dir, tmp_dir, options)
    -- Nothing needed - source is already fetched by git
end
"""
        spec_path = self.cache_root / "ninja_recipe.lua"
        spec_path.write_text(spec_content)

        manifest = test_config.write_spec_manifest(
            self.cache_root, [("test.ninja@v1", spec_path)]
        )
        result = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                *self.trace_flag,
                "install",
                "--manifest",
                str(manifest),
            ],
            capture_output=True,
            text=True,
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertIn("test.ninja@v1", result.stderr)

    # ========================================================================
    # Parallel Git Fetching
    # ========================================================================

    def test_parallel_git_fetch(self):
        """Spec with multiple git sources fetches concurrently (programmatic)."""
        # Programmatic fetch with multiple git repos (should parallelize)
        spec = """IDENTITY = "local.fetch_git_parallel@v1"

function FETCH(tmp_dir, options)
    envy.fetch({
        {source = "https://github.com/ninja-build/ninja.git", ref = "v1.13.2"},
        {source = "https://github.com/google/re2.git", ref = "2024-07-02"}
    }, {dest = tmp_dir})
    envy.commit_fetch({"ninja.git", "re2.git"})
end

function INSTALL(install_dir, stage_dir, fetch_dir, tmp_dir, options)
    local repos = {"ninja.git", "re2.git"}
    for _, repo in ipairs(repos) do
        local git_head = fetch_dir .. "/" .. repo .. "/.git/HEAD"
        local f = io.open(git_head, "r")
        if not f then
            error("Parallel git fetch failed: repo not found: " .. repo)
        end
        f:close()
    end
end
"""
        spec_path = self.specs_dir / "fetch_git_parallel.lua"
        spec_path.write_text(spec, encoding="utf-8")

        manifest = test_config.write_spec_manifest(
            self.specs_dir, [("local.fetch_git_parallel@v1", spec_path)]
        )
        result = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                *self.trace_flag,
                "install",
                "--manifest",
                str(manifest),
            ],
            capture_output=True,
            text=True,
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertIn("local.fetch_git_parallel@v1", result.stderr)

    def test_parallel_git_fetch_declarative(self):
        """Spec with multiple git sources fetches concurrently (declarative)."""
        # Declarative fetch with multiple repos triggers parallel fetch
        spec = """IDENTITY = "local.fetch_git_parallel_declarative@v1"

FETCH = {
  {
    source = "https://github.com/ninja-build/ninja.git",
    ref = "v1.13.2"
  },
  {
    source = "https://github.com/google/re2.git",
    ref = "2024-07-02"
  }
}

function INSTALL(install_dir, stage_dir, fetch_dir, tmp_dir, options)
    local repos = {"ninja.git", "re2.git"}
    for _, repo in ipairs(repos) do
        local git_head = stage_dir .. "/" .. repo .. "/.git/HEAD"
        local f = io.open(git_head, "r")
        if not f then
            error("Parallel git fetch failed: repo not found: " .. repo)
        end
        f:close()
    end
end
"""
        spec_path = self.specs_dir / "fetch_git_parallel_declarative.lua"
        spec_path.write_text(spec, encoding="utf-8")

        manifest = test_config.write_spec_manifest(
            self.specs_dir, [("local.fetch_git_parallel_declarative@v1", spec_path)]
        )
        result = test_config.run(
            [
                str(self.envy),
                f"--cache-root={self.cache_root}",
                *self.trace_flag,
                "install",
                "--manifest",
                str(manifest),
            ],
            capture_output=True,
            text=True,
        )

        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")
        self.assertIn("local.fetch_git_parallel_declarative@v1", result.stderr)


if __name__ == "__main__":
    unittest.main()
