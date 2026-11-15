"""Functional tests that files in fetch/ are released before STAGE and INSTALL use them.

Windows enforces share modes, so an open handle on a downloaded file can fail a reader,
a launch, or a rename. Two kinds of holder matter:

- Children envy spawns. Each one used to inherit every inheritable handle in envy,
  including another package's download in progress, and held it until it exited (#401).
- Outside processes such as antivirus or an indexer. STAGE and INSTALL wait for them per
  file and show the wait on the package's row.

The stub origin orders each step with marker files and holds, so the tests are
deterministic.
"""

from __future__ import annotations

import ctypes
import hashlib
import os
import re
import sys
import threading
import time
import unittest
from ctypes import wintypes
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from . import test_config
from .env import EnvyTestCase

PAYLOAD = bytes(range(256)) * 1024  # 256 KiB, sent in two halves by /gated
PAYLOAD_SHA256 = hashlib.sha256(PAYLOAD).hexdigest()
MARKER_TIMEOUT_S = 30.0

GENERIC_READ = 0x80000000
FILE_SHARE_WRITE = 0x2
FILE_SHARE_DELETE = 0x4
OPEN_EXISTING = 3


def _wait_for(path: Path, timeout: float, stop: threading.Event) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline and not stop.is_set():
        if path.exists():
            return True
        time.sleep(0.02)
    return path.exists()


def _open_holder(path: Path, mode: str):
    """Opens `path` the way a holder would and returns a function that closes it.

    "rw": read/write, sharing read and write. Denies delete, as an inherited copy of
    envy's download handle did.
    "noread": read, sharing write and delete. Denies other readers, so 7-Zip fails.
    """
    if mode == "rw":
        return open(path, "r+b").close  # noqa: SIM115 - the caller closes it
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CreateFileW.restype = wintypes.HANDLE
    kernel32.CreateFileW.argtypes = [
        wintypes.LPCWSTR,
        wintypes.DWORD,
        wintypes.DWORD,
        wintypes.LPVOID,
        wintypes.DWORD,
        wintypes.DWORD,
        wintypes.HANDLE,
    ]
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    handle = kernel32.CreateFileW(
        str(path),
        GENERIC_READ,
        FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        None,
        OPEN_EXISTING,
        0,
        None,
    )
    if handle in (None, ctypes.c_void_p(-1).value):
        raise ctypes.WinError(ctypes.get_last_error())
    return lambda: kernel32.CloseHandle(handle)


class _Holds:
    """Open holders, each closed after its delay or at test cleanup."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._closers: list = []
        self._timers: list[threading.Timer] = []

    def add(self, path: Path, mode: str, hold_ms: int) -> None:
        close = _open_holder(path, mode)
        once = threading.Lock()
        closed = []

        def close_once() -> None:
            with once:
                if not closed:
                    closed.append(True)
                    close()

        timer = threading.Timer(hold_ms / 1000, close_once)
        with self._lock:
            self._closers.append(close_once)
            self._timers.append(timer)
        timer.start()

    def release_all(self) -> None:
        with self._lock:
            timers, closers = list(self._timers), list(self._closers)
        for timer in timers:
            timer.cancel()
        for close in closers:
            close()


class _SequencedOrigin(BaseHTTPRequestHandler):
    """Routes:

    /gated/<name>: sends half the body, touches `started`, waits for `spawned`, then
        sends the rest. Anything a sibling spawns between the markers starts while the
        download is in progress. Records whether `spawned` appeared in `gate_ok`.
    /held/<mode>/<ms>/<name>: sends all but the last byte, opens `hold_path()` with
        _open_holder(mode) for <ms>, then sends the last byte. The file is still held
        when STAGE starts. Failures land in `errors` for the test thread to report.
    """

    protocol_version = "HTTP/1.1"

    def log_message(self, fmt: str, *args: object) -> None:  # noqa: A003
        return

    def _head(self) -> None:
        self.send_response(200)
        self.send_header("Content-Length", str(len(PAYLOAD)))
        self.end_headers()

    def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        server = self.server
        parts = self.path.strip("/").split("/")
        with server.lock:
            server.requests.append(self.path)

        if parts[0] == "gated":
            half = len(PAYLOAD) // 2
            self._head()
            self.wfile.write(PAYLOAD[:half])
            self.wfile.flush()
            (server.markers / "started").touch()
            server.gate_ok = _wait_for(
                server.markers / "spawned", MARKER_TIMEOUT_S, server.closing
            )
            self.wfile.write(PAYLOAD[half:])
            self.wfile.flush()
            return

        if parts[0] == "held":
            self._head()
            self.wfile.write(PAYLOAD[:-1])
            self.wfile.flush()
            # envy has the destination open for write, sharing read and write. Both
            # holder modes share write, so they open alongside it.
            try:
                server.holds.add(server.hold_path(), parts[1], int(parts[2]))
            except Exception as e:  # noqa: BLE001 - reported by the test thread
                with server.lock:
                    server.errors.append(repr(e))
            self.wfile.write(PAYLOAD[-1:])
            self.wfile.flush()
            return

        self.send_error(404, "Not Found")


@unittest.skipUnless(sys.platform == "win32", "share-mode holds are Windows-only")
class FetchReleaseTest(EnvyTestCase):
    envy_watchdog_timeout = 90

    def setUp(self) -> None:
        super().setUp()
        self.markers = self.make_temp_dir("markers")
        server = ThreadingHTTPServer(("127.0.0.1", 0), _SequencedOrigin)
        server.markers = self.markers
        server.hold_path = self.fetched_payload
        server.holds = _Holds()
        server.lock = threading.Lock()
        server.requests = []
        server.errors = []
        server.gate_ok = None
        server.closing = threading.Event()
        self.origin = server
        thread = test_config.serve_in_thread(server)

        def stop():
            server.closing.set()
            server.shutdown()
            server.server_close()
            thread.join(timeout=10)
            if thread.is_alive():
                raise AssertionError("stub origin did not stop within 10s")

        self.addCleanup(stop)
        # Registered after the temp dirs, so it runs before their removal.
        self.addCleanup(server.holds.release_all)
        self.base_url = f"http://127.0.0.1:{server.server_address[1]}"
        self.pid_tag = f"(pid {os.getpid()})"

    def fetched_payload(self) -> Path:
        """payload.bin in the scratch cache's fetch dir. Raises when absent."""
        return next(self.cache_root.glob("packages/**/fetch/payload.bin"))

    def install_one(self, identity: str, body: str, **kwargs):
        spec = self.write_spec(f"{identity.split('@')[0]}.lua", body)
        manifest = self.write_manifest(
            "PACKAGES = {\n" + test_config.spec_entry(identity, spec) + "\n}\n"
        )
        return manifest, self.install(manifest, **kwargs)

    def held_spec(self, identity: str, route: str, sha256: bool = True) -> str:
        verify = f', sha256 = "{PAYLOAD_SHA256}"' if sha256 else ""
        return f"""IDENTITY = "{identity}"
FETCH = {{ source = "{self.base_url}/{route}/payload.bin"{verify} }}
STAGE = function(fetch_dir, stage_dir, tmp_dir, opts) end
INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, opts) end
"""

    def short_budget(self) -> dict[str, str]:
        env = test_config.get_test_env()
        env["ENVY_FETCH_RELEASE_BUDGET_MS"] = "300"
        return env

    def assert_origin_ok(self) -> None:
        self.assertEqual([], self.origin.errors, "stub origin failed")

    def test_sibling_child_does_not_inherit_download(self) -> None:
        """A shell spawned during a download leaves the downloaded file closed.

        `bystander` waits until `download` is half done, then spawns a shell that runs
        past the end of the download and touches `child_done` when it exits. `download`
        renames its fetched file, which needs every open handle to share delete, and
        fails if `child_done` already exists. An inherited copy of envy's write handle
        shares only read and write, so with inheritance the rename either fails or waits
        for the child to exit, and both fail the test.
        """
        started = self.lua_path(self.markers / "started")
        spawned = self.lua_path(self.markers / "spawned")
        child_done = self.lua_path(self.markers / "child_done")

        download = self.write_spec(
            "download.lua",
            f"""IDENTITY = "local.release_download@v1"
FETCH = {{ source = "{self.base_url}/gated/payload.bin" }}
STAGE = function(fetch_dir, stage_dir, tmp_dir, opts) end
INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, opts)
  local done = io.open([[{child_done}]], "r")
  if done then done:close() error("the rename ran after the child exited") end
  local ok, err = os.rename(fetch_dir .. "payload.bin", install_dir .. "payload.bin")
  if not ok then error("rename of fetched file failed: " .. tostring(err)) end
end
""",
        )
        bystander = self.write_spec(
            "bystander.lua",
            f"""IDENTITY = "local.release_bystander@v1"
FETCH = function(tmp_dir, opts) end
INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, opts)
  local deadline = os.time() + {int(MARKER_TIMEOUT_S)}
  while true do
    local f = io.open([[{started}]], "r")
    if f then f:close() break end
    if os.time() > deadline then error("download never started") end
  end
  envy.run([[New-Item -ItemType File -Force -Path '{spawned}' | Out-Null
Start-Sleep -Seconds 6
New-Item -ItemType File -Force -Path '{child_done}' | Out-Null]],
           {{ shell = ENVY_SHELL.POWERSHELL }})
end
""",
        )
        manifest = self.write_manifest(
            "PACKAGES = {\n"
            + test_config.spec_entry("local.release_download@v1", download)
            + "\n"
            + test_config.spec_entry("local.release_bystander@v1", bystander)
            + "\n}\n"
        )

        run = self.install(manifest)

        self.assertEqual(0, run.returncode, f"stdout: {run.stdout}\nstderr: {run.stderr}")
        self.assertTrue(self.origin.gate_ok, "bystander spawned after the download ended")
        entries = self.pkg_entries("local.release_download@v1")
        installed = [f for e in entries for f in e.rglob("payload.bin")]
        self.assertEqual(1, len(installed), f"expected one installed payload in {entries}")
        self.assertEqual(PAYLOAD, installed[0].read_bytes())
        powershell_waits = [
            e
            for e in run.events("fetch_release_wait")
            if "powershell" in e.raw["holders"].lower()
        ]
        self.assertEqual([], powershell_waits)

    def test_stage_awaits_outside_holder(self) -> None:
        """STAGE waits for a holder that outlasts the download and names it."""
        _, run = self.install_one(
            "local.release_rw@v1", self.held_spec("local.release_rw@v1", "held/rw/2000")
        )

        self.assert_origin_ok()
        self.assertEqual(0, run.returncode, f"stderr: {run.stderr}")
        waits = run.events("fetch_release_wait")
        self.assertEqual(1, len(waits), f"expected one release wait, got {waits}")
        self.assertTrue(waits[0].raw["released"])
        self.assertGreaterEqual(waits[0].raw["duration_ms"], 1000)
        self.assertIn(self.pid_tag, waits[0].raw["holders"])
        # A completed install deletes fetch/, so compare the path's tail.
        self.assertTrue(
            waits[0].raw["path"].endswith(os.path.join("fetch", "payload.bin")),
            waits[0].raw["path"],
        )

    def test_stage_awaits_holder_that_denies_readers(self) -> None:
        """A holder that shares delete but denies read sharing also counts as held.

        The spec omits sha256. Verifying would read the file during fetch, and this
        holder blocks that read before STAGE runs.
        """
        _, run = self.install_one(
            "local.release_noread@v1",
            self.held_spec("local.release_noread@v1", "held/noread/2000", sha256=False),
        )

        self.assert_origin_ok()
        self.assertEqual(0, run.returncode, f"stderr: {run.stderr}")
        waits = run.events("fetch_release_wait")
        self.assertEqual(1, len(waits), f"expected one release wait, got {waits}")
        self.assertGreaterEqual(waits[0].raw["duration_ms"], 1000)

    def test_stage_fails_once_release_budget_is_spent(self) -> None:
        """Past the budget, the install fails and the error names the holder."""
        _, run = self.install_one(
            "local.release_budget@v1",
            self.held_spec("local.release_budget@v1", "held/rw/30000"),
            env=self.short_budget(),
        )

        self.assert_origin_ok()
        self.assertNotEqual(0, run.returncode, f"stdout: {run.stdout}")
        self.assertRegex(
            run.stderr,
            r"payload\.bin still held by .*"
            + re.escape(self.pid_tag)
            + r".* after \d+\.\ds \(ENVY_FETCH_RELEASE_BUDGET_MS\)",
        )
        waits = run.events("fetch_release_wait")
        self.assertEqual(1, len(waits), f"expected one release wait, got {waits}")
        self.assertFalse(waits[0].raw["released"])

    def test_cached_file_is_released_before_reuse(self) -> None:
        """A file kept in fetch/ after a failed release wait is checked again on reuse.

        The first install downloads and verifies the file, then times out on the holder
        in STAGE. The second install reuses the completed fetch dir while a holder is
        still open, and fails the same way. The third, with the holder closed, installs
        without downloading again.
        """
        manifest, first = self.install_one(
            "local.release_held@v1",
            self.held_spec("local.release_held@v1", "held/rw/30000"),
            env=self.short_budget(),
        )
        self.assert_origin_ok()
        self.assertNotEqual(0, first.returncode, f"stdout: {first.stdout}")
        self.assertIn("still held", first.stderr)

        second = self.install(manifest, env=self.short_budget())
        self.assertNotEqual(0, second.returncode, f"stdout: {second.stdout}")
        self.assertIn("still held", second.stderr)
        self.assertEqual(1, len(self.origin.requests), "the retry downloaded again")

        self.origin.holds.release_all()
        third = self.install(manifest)
        self.assertEqual(0, third.returncode, f"stderr: {third.stderr}")
        self.assertEqual(1, len(self.origin.requests), "the cached file was not reused")
        self.assertTrue(self.pkg_complete("local.release_held@v1"))

    def test_commit_fetch_fails_fast_on_a_file_envy_holds(self) -> None:
        """commit_fetch fails at once on a file this envy still has open.

        Waiting would last the whole budget, since the handle belongs to the thread
        that is waiting.
        """
        _, run = self.install_one(
            "local.release_self@v1",
            """IDENTITY = "local.release_self@v1"
FETCH = function(tmp_dir, opts)
  held = io.open(tmp_dir .. "payload.bin", "wb")
  held:write("payload")
  envy.commit_fetch("payload.bin")
end
""",
        )

        self.assertNotEqual(0, run.returncode, f"stdout: {run.stdout}")
        self.assertIn("payload.bin is held open by this envy process", run.stderr)
        waits = run.events("fetch_release_wait")
        self.assertEqual(1, len(waits), f"expected one release wait, got {waits}")
        self.assertFalse(waits[0].raw["released"])
        self.assertLess(waits[0].raw["duration_ms"], 30000)

    def test_commit_fetch_checks_each_file_in_a_directory(self) -> None:
        """commit_fetch of a directory checks the files inside it."""
        _, run = self.install_one(
            "local.release_tree@v1",
            """IDENTITY = "local.release_tree@v1"
FETCH = function(tmp_dir, opts)
  envy.run("New-Item -ItemType Directory -Force lib | Out-Null",
           { shell = ENVY_SHELL.POWERSHELL })
  held = io.open(tmp_dir .. "lib/inner.bin", "wb")
  held:write("payload")
  envy.commit_fetch("lib")
end
""",
        )

        self.assertNotEqual(0, run.returncode, f"stdout: {run.stdout}")
        self.assertIn("inner.bin is held open by this envy process", run.stderr)


if __name__ == "__main__":
    unittest.main()
