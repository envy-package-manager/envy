"""A git fetch must keep a progress bar for the whole clone.

libgit2 reports two phases through one callback: objects arriving, then the pack's
deltas resolving. Dropping the bar when the last object lands left the row showing a
bare "N/N objects" count for the rest of the clone.

The repo is served by `git daemon` out of the fixture's own .git, because libgit2's
local transport never invokes the transfer-progress callback at all, so a file://
clone would render nothing.
Assertions read the fallback renderer's output (TERM=dumb, throttle forced to 0),
where a progress row ends in ": NN.N%" and a bar-less row does not.
"""

import os
import re
import shutil
import socket
import subprocess
import threading
import time
import unittest
from pathlib import Path

from . import test_config
from .env import EnvyTestCase
from .test_config import make_manifest

_GIT = shutil.which("git")

BUNDLE_LUA = """BUNDLE = "test.gitprog@v1"
SPECS = { ["test.tool@v1"] = "tool.lua" }
"""

TOOL_SPEC = """IDENTITY = "test.tool@v1"
DEPENDENCIES = {}
USER_MANAGED = true
SETUP = {
  main = {
    CHECK = function(pkg_dir, options) return false end,
    INSTALL = function(pkg_dir, options) end,
  },
}
"""


@unittest.skipIf(_GIT is None, "git binary not available")
class TestGitFetchProgress(EnvyTestCase):
    def setUp(self):
        super().setUp()

        repo = self.work / "src"
        repo.mkdir()
        (repo / "envy-bundle.lua").write_text(BUNDLE_LUA, encoding="utf-8")
        (repo / "tool.lua").write_text(TOOL_SPEC, encoding="utf-8")

        # Enough objects to span several render cycles, and a second commit that
        # rewrites them so the pack carries deltas to resolve.
        for i in range(150):
            (repo / f"blob{i}.bin").write_bytes(os.urandom(48 * 1024))
        self._git(repo, "init", "-b", "main")
        self._git(repo, "add", "-A")
        self._git(repo, "commit", "-m", "one")
        for i in range(150):
            with open(repo / f"blob{i}.bin", "ab") as f:
                f.write(os.urandom(4 * 1024))
        self._git(repo, "add", "-A")
        self._git(repo, "commit", "-m", "two")

        sock = socket.socket()
        sock.bind(("127.0.0.1", 0))
        self.port = sock.getsockname()[1]
        sock.close()
        # Serves src/.git directly: a `git clone --bare` staging copy held the same
        # loose objects upload-pack repacks per fetch, for one more killable process.
        self.daemon = subprocess.Popen(
            [
                _GIT,
                "daemon",
                f"--port={self.port}",
                "--reuseaddr",
                "--export-all",
                f"--base-path={self.work}",
                str(self.work),
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        self._await_daemon()

    def tearDown(self):
        self.daemon.terminate()
        self.daemon.wait(timeout=10)

    def _git(self, cwd: Path, *args: str):
        """Run git, failing with git's own diagnosis rather than a bare exit code.

        check=True raises CalledProcessError, whose message is the argv and a number,
        so a fixture that dies on a busy runner says nothing about why.
        """
        run = subprocess.run(
            [
                _GIT,
                "-c",
                "user.name=envy-test",
                "-c",
                "user.email=envy@test.invalid",
                *args,
            ],
            cwd=cwd,
            capture_output=True,
            text=True,
        )
        if run.returncode != 0:
            self.fail(f"git {' '.join(args)} failed ({run.returncode}): "
                      f"{run.stderr.strip()}")

    def _await_daemon(self, timeout: float = 10.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            with socket.socket() as s:
                s.settimeout(0.25)
                if s.connect_ex(("127.0.0.1", self.port)) == 0:
                    return
            time.sleep(0.05)
        self.fail("git daemon did not start")

    def _install(self) -> subprocess.CompletedProcess:
        manifest = self.work / "envy.lua"
        manifest.write_text(
            make_manifest(
                f"""
BUNDLES = {{
    tc = {{ identity = "test.gitprog@v1",
           source = "git://127.0.0.1:{self.port}/src/.git", ref = "main" }},
}}

PACKAGES = {{
    {{ spec = "test.tool@v1", bundle = "tc", setup = {{ "main" }} }},
}}
"""
            ),
            encoding="utf-8",
        )

        env = test_config.get_test_env()
        env["TERM"] = "dumb"
        env["ENVY_TEST_FALLBACK_THROTTLE_MS"] = "0"
        return test_config.run(
            [
                str(self.envy),
                "--cache-root",
                str(self.cache_root),
                "install",
                "--manifest",
                str(manifest),
            ],
            cwd=self.project_root,
            capture_output=True,
            text=True,
            env=env,
        )

    def test_git_clone_rows_always_carry_a_bar(self):
        result = self._install()
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        rows = [
            ln.strip()
            for ln in result.stderr.splitlines()
            if "test.gitprog@v1" in ln and ("objects" in ln or "deltas" in ln)
        ]
        self.assertTrue(rows, f"no git progress rows rendered: {result.stderr}")

        # A progress row carries a percentage; a bar-less status line does not.
        barless = [ln for ln in rows if not re.search(r":\s*\d+\.\d%$", ln)]
        self.assertEqual([], barless, f"rows rendered without a bar: {barless}")

        # The bundle still finishes with its own outcome row.
        self.assertRegex(result.stderr, r"\[test\.gitprog@v1\] fetched \(\d+\.\ds\)")

    def test_git_clone_reports_delta_resolution(self):
        """After the last object arrives the bar tracks delta resolution."""
        result = self._install()
        self.assertEqual(result.returncode, 0, f"stderr: {result.stderr}")

        object_rows = [ln for ln in result.stderr.splitlines() if "objects" in ln]
        self.assertTrue(object_rows, f"no object rows: {result.stderr}")

        # Nothing may report a completed object count without a live bar; that was the
        # state that used to persist for the rest of the clone.
        finished_without_bar = [
            ln
            for ln in object_rows
            if re.search(r"(\d+)/\1 objects", ln) and not re.search(r"\d+\.\d%$", ln)
        ]
        self.assertEqual([], finished_without_bar, finished_without_bar)


if __name__ == "__main__":
    unittest.main()
