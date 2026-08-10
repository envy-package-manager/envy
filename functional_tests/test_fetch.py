from __future__ import annotations

import os
import re
import subprocess
import tempfile
import threading
import time
from functools import partial
from http.server import (
    BaseHTTPRequestHandler,
    SimpleHTTPRequestHandler,
    ThreadingHTTPServer,
)
from pathlib import Path
import unittest

from . import test_config


class _QuietHandler(SimpleHTTPRequestHandler):
    def __init__(self, *args, directory: str | None = None, **kwargs):
        super().__init__(*args, directory=directory, **kwargs)

    def log_message(self, format: str, *args: object) -> None:  # noqa: A003
        # Suppress per-request logging to keep test output clean.
        return


class FetchCommandFunctionalTest(unittest.TestCase):
    def setUp(self) -> None:
        self._envy_binary = test_config.get_envy_production_executable()
        self._project_root = Path(__file__).resolve().parent.parent

    def test_fetch_local_file(self) -> None:
        """Test fetch with local file source."""

        with tempfile.TemporaryDirectory() as temp_dir:
            source_file = Path(temp_dir) / "source.txt"
            payload = b"local file content\n"
            source_file.write_bytes(payload)

            destination = Path(temp_dir) / "dest" / "output.txt"

            env = os.environ.copy()
            env.setdefault("ENVY_CACHE_DIR", str(self._project_root / "out" / "cache"))

            result = test_config.run(
                [str(self._envy_binary), "fetch", str(source_file), str(destination)],
                check=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                env=env,
            )

            self.assertEqual("", result.stdout)
            self.assertTrue(
                destination.exists(), f"expected {destination} to exist after fetch"
            )
            self.assertEqual(payload, destination.read_bytes())

    def test_fetch_local_directory(self) -> None:
        """Test fetch with local directory source (recursive copy)."""
        self.assertTrue(
            self._envy_binary.exists(), f"envy binary missing at {self._envy_binary}"
        )

        with tempfile.TemporaryDirectory() as temp_dir:
            source_dir = Path(temp_dir) / "source"
            source_dir.mkdir()
            (source_dir / "file1.txt").write_text("content 1")
            (source_dir / "file2.txt").write_text("content 2")
            subdir = source_dir / "subdir"
            subdir.mkdir()
            (subdir / "nested.txt").write_text("nested content")

            destination = Path(temp_dir) / "dest"

            env = os.environ.copy()
            env.setdefault("ENVY_CACHE_DIR", str(self._project_root / "out" / "cache"))

            result = test_config.run(
                [str(self._envy_binary), "fetch", str(source_dir), str(destination)],
                check=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                env=env,
            )

            self.assertEqual("", result.stdout)
            self.assertTrue(
                destination.exists(), f"expected {destination} to exist after fetch"
            )
            self.assertTrue((destination / "file1.txt").exists())
            self.assertTrue((destination / "file2.txt").exists())
            self.assertTrue((destination / "subdir" / "nested.txt").exists())
            self.assertEqual("content 1", (destination / "file1.txt").read_text())
            self.assertEqual(
                "nested content", (destination / "subdir" / "nested.txt").read_text()
            )

    def test_fetch_local_file_nonexistent(self) -> None:
        """Test fetch fails gracefully when source doesn't exist."""
        self.assertTrue(
            self._envy_binary.exists(), f"envy binary missing at {self._envy_binary}"
        )

        with tempfile.TemporaryDirectory() as temp_dir:
            source = Path(temp_dir) / "nonexistent.txt"
            destination = Path(temp_dir) / "dest.txt"

            env = os.environ.copy()
            env.setdefault("ENVY_CACHE_DIR", str(self._project_root / "out" / "cache"))

            result = test_config.run(
                [str(self._envy_binary), "fetch", str(source), str(destination)],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                env=env,
            )

            self.assertNotEqual(
                0, result.returncode, "expected fetch to fail for nonexistent source"
            )
            self.assertIn("does not exist", result.stderr.lower())

    def test_fetch_http_download(self) -> None:
        self.assertTrue(
            self._envy_binary.exists(), f"envy binary missing at {self._envy_binary}"
        )

        with tempfile.TemporaryDirectory() as temp_dir:
            serve_dir = Path(temp_dir) / "serve"
            serve_dir.mkdir(parents=True, exist_ok=True)
            payload = b"hello from envy fetch\n"
            payload_name = "payload.txt"
            (serve_dir / payload_name).write_bytes(payload)

            handler = partial(_QuietHandler, directory=str(serve_dir))
            server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
            server_thread = threading.Thread(target=server.serve_forever, daemon=True)
            server_thread.start()
            try:
                port = server.server_address[1]
                url = f"http://127.0.0.1:{port}/{payload_name}"
                destination = Path(temp_dir) / "download" / "out.txt"

                env = os.environ.copy()
                env.setdefault(
                    "ENVY_CACHE_DIR", str(self._project_root / "out" / "cache")
                )

                result = test_config.run(
                    [str(self._envy_binary), "fetch", url, str(destination)],
                    check=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    env=env,
                )

                self.assertEqual("", result.stdout)
                self.assertTrue(
                    destination.exists(), f"expected {destination} to exist after fetch"
                )
                self.assertEqual(payload, destination.read_bytes())
            finally:
                server.shutdown()
                server_thread.join(timeout=5)
                server.server_close()

    def test_fetch_http_draws_a_progress_bar(self) -> None:
        """Anything downloaded reports a progress bar, commands included.

        Served in throttled chunks so the fallback renderer (TERM=dumb, throttle
        forced to 0) gets a cycle mid-transfer; a row it draws carries ": NN.N%".
        """
        # The bar is drawn by the functional tester, which honors the throttle override.
        envy = test_config.get_envy_executable()
        payload = os.urandom(256 * 1024)

        class _SlowHandler(BaseHTTPRequestHandler):
            def log_message(self, fmt: str, *args: object) -> None:  # noqa: A003
                return

            def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
                self.send_response(200)
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                chunk = len(payload) // 8
                for start in range(0, len(payload), chunk):
                    self.wfile.write(payload[start : start + chunk])
                    self.wfile.flush()
                    time.sleep(0.25)

        server = ThreadingHTTPServer(("127.0.0.1", 0), _SlowHandler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with tempfile.TemporaryDirectory() as temp_dir:
                url = f"http://127.0.0.1:{server.server_address[1]}/payload.bin"
                destination = Path(temp_dir) / "out.bin"

                env = test_config.get_test_env()
                env["TERM"] = "dumb"
                env["ENVY_TEST_FALLBACK_THROTTLE_MS"] = "0"
                result = test_config.run(
                    [str(envy), "fetch", url, str(destination)],
                    capture_output=True,
                    text=True,
                    env=env,
                )

                self.assertEqual(0, result.returncode, f"stderr: {result.stderr}")
                self.assertEqual(payload, destination.read_bytes())

                rows = [
                    ln.strip()
                    for ln in result.stderr.splitlines()
                    if re.search(r":\s*\d+\.\d%$", ln)
                ]
                self.assertTrue(rows, f"no progress row rendered: {result.stderr}")
                self.assertTrue(
                    any(url in ln for ln in rows),
                    f"progress rows do not name the source: {rows}",
                )
        finally:
            server.shutdown()
            thread.join(timeout=5)
            server.server_close()


if __name__ == "__main__":
    unittest.main()
