"""Tests for the functional-test harness itself (functional_tests/test_config.py).

The suite starts hundreds of stub HTTP servers, and every one is shut down in a cleanup.
serve_forever only notices shutdown() once per poll interval, so that interval is paid on
every teardown; at the 0.5s default it was a third of the suite's runtime.
"""

from __future__ import annotations

import time
import unittest
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from . import test_config


class _Quiet(BaseHTTPRequestHandler):
    def log_message(self, format: str, *args: object) -> None:
        pass


class ServeInThreadTest(unittest.TestCase):
    def test_shutdown_does_not_wait_out_the_default_poll(self) -> None:
        server = ThreadingHTTPServer(("127.0.0.1", 0), _Quiet)
        thread = test_config.serve_in_thread(server)
        # Answered request: the loop is back in its poll wait when shutdown() arrives.
        url = f"http://127.0.0.1:{server.server_address[1]}/"
        with self.assertRaises(urllib.error.HTTPError):
            urllib.request.urlopen(url, timeout=5)

        start = time.monotonic()
        server.shutdown()
        elapsed = time.monotonic() - start
        server.server_close()
        thread.join(timeout=5)

        self.assertFalse(thread.is_alive())
        self.assertLess(elapsed, 0.25, f"shutdown() took {elapsed:.3f}s")


if __name__ == "__main__":
    unittest.main()
