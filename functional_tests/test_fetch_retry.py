"""Functional tests for fetch retry, backoff, and transport error reporting.

A stub origin decides per request whether to fail, so one fixed URL can be flaky
exactly as many times as a case needs. All assertions read the trace rather than
log prose: `download_retry` carries the attempt, the jittered delay, and the
`fetch_error_kind` that earned the retry.
"""

from __future__ import annotations

import threading
from collections import defaultdict
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from . import test_config
from .env import EnvyTestCase

PAYLOAD = b"envy retry payload, long enough to truncate in half\n"

# Short enough that three attempts cost milliseconds, non-zero so the jitter under
# test still has a range to spread over.
RETRY_BASE_MS = "20"


class _FlakyOrigin(BaseHTTPRequestHandler):
    """Routes: /transient/<n>/<name>, /truncate/<n>/<name>, /redirect/<name>, /missing.

    <n> is how many times that exact path fails before it starts working, so a case
    picks its own failure budget and concurrent paths never share a counter.
    """

    protocol_version = "HTTP/1.1"

    def log_message(self, fmt: str, *args: object) -> None:  # noqa: A003
        return

    def _nth_attempt(self) -> int:
        """1-based count of requests this path has now seen.

        The tally lives on the server, not the handler class: the suite runs test
        methods of one class in parallel threads of a single process, so class-level
        state would have each setUp resetting a sibling test's counters mid-run.
        """
        with self.server.attempts_lock:
            self.server.attempts[self.path] += 1
            return self.server.attempts[self.path]

    def _send_payload(self) -> None:
        self.send_response(200)
        self.send_header("Content-Length", str(len(PAYLOAD)))
        self.end_headers()
        self.wfile.write(PAYLOAD)

    def _send_half_then_hang_up(self) -> None:
        # Announce the full length, deliver half, drop the connection: the shape of a
        # mirror that accepts the request and then stops sending.
        self.send_response(200)
        self.send_header("Content-Length", str(len(PAYLOAD)))
        self.end_headers()
        self.wfile.write(PAYLOAD[: len(PAYLOAD) // 2])
        self.wfile.flush()
        self.close_connection = True

    def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        parts = self.path.strip("/").split("/")

        if parts[0] == "missing":
            self.send_error(404, "Not Found")
            return

        if parts[0] == "redirect":
            self.send_response(302)
            # A per-request mirror, exactly as SourceForge hands out: the name that
            # actually fails appears nowhere in the URL envy was asked to fetch.
            self.send_header("Location", f"/truncate/99/{parts[1]}")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return

        failures = int(parts[1])
        if self._nth_attempt() > failures:
            self._send_payload()
        elif parts[0] == "truncate":
            self._send_half_then_hang_up()
        else:
            self.send_error(503, "Service Unavailable")


class FetchRetryTest(EnvyTestCase):
    def setUp(self) -> None:
        super().setUp()
        server = ThreadingHTTPServer(("127.0.0.1", 0), _FlakyOrigin)
        server.attempts = defaultdict(int)
        server.attempts_lock = threading.Lock()
        self.origin = server

        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()

        def stop():
            server.shutdown()
            server.server_close()
            thread.join(timeout=10)

        self.addCleanup(stop)
        self.base_url = f"http://127.0.0.1:{server.server_address[1]}"

    # -- helpers ------------------------------------------------------------

    def fetch(self, path: str, **overrides) -> tuple:
        """`envy fetch <base>/<path>` into scratch; returns (run, destination)."""
        env = test_config.get_test_env()
        # The suite disables retry globally; this file is where the policy is the
        # subject, so it opts back in to the shipping default.
        env["ENVY_FETCH_ATTEMPTS"] = "3"
        env["ENVY_FETCH_RETRY_BASE_MS"] = RETRY_BASE_MS
        env.update(overrides)

        destination = self.work / "out" / Path(path).name
        run = self.run_envy(
            "fetch", f"{self.base_url}/{path}", str(destination), env=env
        )
        return run, destination

    def retries(self, run) -> list:
        return run.events("download_retry")

    # -- cases --------------------------------------------------------------

    def test_transient_5xx_recovers_without_user_retry(self) -> None:
        """Two 503s then a good body: envy rides it out inside one invocation."""
        run, destination = self.fetch("transient/2/pkg.bin")

        self.assertEqual(0, run.returncode, f"stderr: {run.stderr}")
        self.assertEqual(PAYLOAD, destination.read_bytes())

        retries = self.retries(run)
        self.assertEqual(2, len(retries), f"expected 2 retries, got {retries}")
        self.assertEqual([1, 2], [e.raw["attempt"] for e in retries])
        self.assertEqual(
            ["http_status", "http_status"], [e.raw["reason"] for e in retries]
        )

    def test_mid_body_truncation_retries(self) -> None:
        """A body that stops short is a transport fault, not a bad request."""
        run, destination = self.fetch("truncate/1/pkg.bin")

        self.assertEqual(0, run.returncode, f"stderr: {run.stderr}")
        self.assertEqual(PAYLOAD, destination.read_bytes())

        retries = self.retries(run)
        self.assertEqual(1, len(retries), f"expected 1 retry, got {retries}")
        self.assertEqual("transfer", retries[0].raw["reason"])

    def test_404_fails_on_the_first_attempt(self) -> None:
        """Nothing about a replay makes a missing file exist."""
        run, destination = self.fetch("missing")

        self.assertNotEqual(0, run.returncode)
        self.assertFalse(destination.exists())
        self.assertEqual([], self.retries(run), "a 404 must not be retried")
        self.assertEqual(1, len(run.events("download_failed")))

    def test_retries_are_bounded_and_report_the_last_failure(self) -> None:
        """Three attempts total, then the failure surfaces to the user."""
        run, destination = self.fetch("transient/99/pkg.bin")

        self.assertNotEqual(0, run.returncode)
        self.assertFalse(destination.exists())
        self.assertEqual(2, len(self.retries(run)), "3 attempts means 2 retries")
        self.assertIn("503", run.stderr)

        self.assertEqual(
            {"/transient/99/pkg.bin": 3},
            dict(self.origin.attempts),
            "the origin should have been asked exactly three times",
        )

    def test_attempt_count_is_configurable(self) -> None:
        """ENVY_FETCH_ATTEMPTS=1 opts out of retry entirely."""
        run, _ = self.fetch("transient/99/pkg.bin", ENVY_FETCH_ATTEMPTS="1")

        self.assertNotEqual(0, run.returncode)
        self.assertEqual([], self.retries(run))

    def test_error_names_the_bytes_and_the_post_redirect_url(self) -> None:
        """The mirror a redirect picked is the one that failed; name it, and say
        how far it got. The requested URL alone identifies nobody."""
        run, _ = self.fetch("redirect/pkg.bin")

        self.assertNotEqual(0, run.returncode)
        self.assertIn(f" of {len(PAYLOAD)} bytes from ", run.stderr)
        self.assertIn(f"{self.base_url}/truncate/99/pkg.bin", run.stderr)

    def test_concurrent_retries_do_not_run_in_lockstep(self) -> None:
        """fetch() runs a thread per request. Unjittered backoff would march every
        one of them back onto the same bad mirror at the same instant."""
        count = 8
        sources = "\n".join(
            f'  {{ source = "{self.base_url}/transient/1/item{i}.bin" }},'
            for i in range(count)
        )
        spec = self.write_spec(
            "flaky.lua",
            f'IDENTITY = "local.flaky@v1"\nFETCH = {{\n{sources}\n}}\n',
        )
        manifest = test_config.write_spec_manifest(
            self.work, [("local.flaky@v1", spec)]
        )

        env = test_config.get_test_env()
        env["ENVY_FETCH_ATTEMPTS"] = "3"
        env["ENVY_FETCH_RETRY_BASE_MS"] = RETRY_BASE_MS
        run = self.run_envy("install", "--manifest", str(manifest), env=env)

        self.assertEqual(0, run.returncode, f"stderr: {run.stderr}")

        retries = self.retries(run)
        self.assertEqual(count, len(retries), f"expected {count} retries: {retries}")

        delays = {e.raw["delay_ms"] for e in retries}
        self.assertGreater(
            len(delays), 1, f"all {count} retries waited the same {delays}"
        )
