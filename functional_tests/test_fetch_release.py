"""Functional tests: a fetched file is released before anything downstream touches it.

Windows enforces share modes, so any handle still open on a just-downloaded file can
fail a reader, a launch, or a rename. Two holders matter:

- envy's own children. Every shell envy spawns used to inherit every inheritable handle
  in the process -- including a sibling package's half-written download -- and kept it
  until it exited (#401).
- Outside processes (antivirus, indexer). fetch waits them out per file, on the row.

The stub origin sequences each case exactly, so neither depends on timing luck.
"""

from __future__ import annotations

import sys
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from . import test_config
from .env import EnvyTestCase

PAYLOAD = bytes(range(256)) * 1024  # 256 KiB: big enough to send in two halves
MARKER_TIMEOUT_S = 30.0


def _wait_for(path: Path, timeout: float) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists():
            return True
        time.sleep(0.02)
    return False


class _SequencedOrigin(BaseHTTPRequestHandler):
    """Routes:

    /gated/<name>: half the body, touch `started`, wait for `spawned`, then the rest --
        so whatever a sibling spawns between the two markers is born mid-download.
    /held/<ms>/<name>: all but the last byte, open `hold_path` read/write for <ms>,
        then the last byte -- so envy's release probe meets a holder every time.
    """

    protocol_version = "HTTP/1.1"

    def log_message(self, fmt: str, *args: object) -> None:  # noqa: A003
        return

    def _head(self) -> None:
        self.send_response(200)
        self.send_header("Content-Length", str(len(PAYLOAD)))
        self.end_headers()

    def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        parts = self.path.strip("/").split("/")
        markers: Path = self.server.markers

        if parts[0] == "gated":
            half = len(PAYLOAD) // 2
            self._head()
            self.wfile.write(PAYLOAD[:half])
            self.wfile.flush()
            (markers / "started").touch()
            _wait_for(markers / "spawned", MARKER_TIMEOUT_S)
            self.wfile.write(PAYLOAD[half:])
            self.wfile.flush()
            return

        if parts[0] == "held":
            hold_ms = int(parts[1])
            self._head()
            self.wfile.write(PAYLOAD[:-1])
            self.wfile.flush()
            # The destination is already open in envy (for write, sharing read/write), so
            # a read/write open sharing the same succeeds; it denies delete, which is
            # what an in-flight scan or an inherited handle does to the next consumer.
            holder = open(self.server.hold_path, "r+b")  # noqa: SIM115 - closed by timer
            threading.Timer(hold_ms / 1000, holder.close).start()
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
        server.hold_path = None
        self.origin = server
        thread = test_config.serve_in_thread(server)

        def stop():
            server.shutdown()
            server.server_close()
            thread.join(timeout=10)

        self.addCleanup(stop)
        self.base_url = f"http://127.0.0.1:{server.server_address[1]}"

    def test_sibling_child_does_not_inherit_download(self) -> None:
        """A shell spawned mid-download must not keep the downloaded file open.

        `bystander` waits until `download`'s transfer is half done, then spawns a shell
        that outlives it. `download` then renames its fetched file -- which needs every
        open handle to share delete. An inherited copy of envy's write handle does not.
        """
        started = self.lua_path(self.markers / "started")
        spawned = self.lua_path(self.markers / "spawned")

        download = self.write_spec(
            "download.lua",
            f"""IDENTITY = "local.release_download@v1"
FETCH = {{ source = "{self.base_url}/gated/payload.bin" }}
STAGE = function(fetch_dir, stage_dir, tmp_dir, opts) end
INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, opts)
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
Start-Sleep -Seconds 6]], {{ shell = ENVY_SHELL.POWERSHELL }})
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
        self.assertTrue((self.markers / "spawned").exists(), "bystander never spawned")
        entries = self.pkg_entries("local.release_download@v1")
        installed = [f for e in entries for f in e.rglob("payload.bin")]
        self.assertEqual(1, len(installed), f"expected one installed payload in {entries}")
        self.assertEqual(PAYLOAD, installed[0].read_bytes())
        # Not merely waited out: the release gate would sit through the child's sleep.
        self.assertEqual([], run.events("fetch_release_wait"))

    def test_fetch_awaits_outside_holder(self) -> None:
        """A file held past the transfer is waited out on its row, holder named."""
        destination = self.work / "out" / "payload.bin"
        self.origin.hold_path = destination

        run = self.run_envy(
            "fetch", f"{self.base_url}/held/2000/payload.bin", str(destination)
        )

        self.assertEqual(0, run.returncode, f"stderr: {run.stderr}")
        self.assertEqual(PAYLOAD, destination.read_bytes())
        waits = run.events("fetch_release_wait")
        self.assertEqual(1, len(waits), f"expected one release wait, got {waits}")
        self.assertGreaterEqual(waits[0].raw["duration_ms"], 1000)
        self.assertIn("python", waits[0].raw["holders"].lower())

    def test_fetch_fails_once_release_budget_is_spent(self) -> None:
        """Past the budget envy stops with the holder named, never a downstream riddle."""
        destination = self.work / "out" / "payload.bin"
        self.origin.hold_path = destination
        env = test_config.get_test_env()
        env["ENVY_FETCH_RELEASE_BUDGET_MS"] = "300"

        run = self.run_envy(
            "fetch", f"{self.base_url}/held/4000/payload.bin", str(destination), env=env
        )

        self.assertNotEqual(0, run.returncode, f"stdout: {run.stdout}")
        self.assertIn("still held", run.stderr)
        self.assertIn("python", run.stderr.lower())


if __name__ == "__main__":
    unittest.main()
