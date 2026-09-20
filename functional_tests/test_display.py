"""DISPLAY, and the silence of a run with no work to do.

Two halves of one behavior. A package's progress row is two columns: `[identity]`, then
whatever the row is saying. `DISPLAY` puts spec-chosen text at the head of column two, so
a spec many packages share -- a `github` spec cloning a different repository per entry --
can say which one each row is. And a package that did nothing gets no row at all, so a
second `envy install` over a warm cache paints an empty screen.

The silence is a TTY behavior: off a TTY every package still reports its outcome, because
a log that omits the no-ops cannot be read as a record of the run.
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
import unittest
from pathlib import Path

from . import test_config
from .env import EnvyTestCase

# Cursor moves, wrap toggles and erases -- everything the live region paints that is not
# text. What survives is what a person would have read off the screen.
ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")

PTY_TIMEOUT_S = 120.0


def visible(text: str) -> str:
    return ANSI.sub("", text).replace("\r", "").strip()


class TestDisplay(EnvyTestCase):
    def setUp(self):
        super().setUp()
        self.payload = self.work / "payload.txt"
        self.payload.write_text("hello\n", encoding="utf-8")

    def _source(self) -> str:
        return f'"file://{self.lua_path(self.payload)}"'

    def _spec(self, name: str, identity: str, body: str) -> Path:
        return self.write_spec(
            name,
            f'IDENTITY = "{identity}"\n'
            f"FETCH = {{ source = {self._source()} }}\n"
            f"{body}\n",
        )

    def _run_on_pty(self, *args):
        """Run envy with stderr on a pty; returns (returncode, everything it painted).

        The live region only exists when envy believes it is talking to a terminal, so a
        pipe cannot answer what a person sees. TERM is pinned because a `dumb` one sends
        envy down the same fallback path a pipe does.
        """
        import pty

        primary, secondary = pty.openpty()
        proc = test_config.popen(
            [
                str(self.envy),
                "--cache-root",
                str(self.cache_root),
                *(str(a) for a in args),
            ],
            cwd=str(self.project_root),
            env={**os.environ, "TERM": "xterm-256color"},
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=secondary,
        )
        os.close(secondary)

        chunks: list[bytes] = []
        try:
            while True:
                try:
                    data = os.read(primary, 65536)
                except OSError:  # the last writer closed the pty
                    break
                if not data:
                    break
                chunks.append(data)
        finally:
            os.close(primary)

        proc.wait(timeout=PTY_TIMEOUT_S)
        return proc.returncode, b"".join(chunks).decode("utf-8", "replace")

    # -- DISPLAY -------------------------------------------------------------

    def test_display_string_leads_column_two(self):
        """A literal DISPLAY prints after the identity column, before the outcome."""
        spec = self._spec("d.lua", "local.d@v1", 'DISPLAY = "the payload"')
        manifest = self.write_manifest(
            test_config.write_spec_manifest(self.work, [("local.d@v1", spec)]).read_text()
        )
        run = self.install(manifest)
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertRegex(run.stderr, r"\[local\.d@v1\] the payload installed \(\d")

    def test_display_function_tells_one_spec_s_instances_apart(self):
        """Two entries, one spec: DISPLAY of the options names each row."""
        spec = self._spec(
            "repo.lua",
            "local.repo@r0",
            'OPTIONS = { repo = { type = "string", required = true } }\n'
            "DISPLAY = function(options) return options.repo end",
        )
        manifest = self.write_manifest(
            "PACKAGES = {\n"
            + test_config.spec_entry(
                "local.repo@r0", spec, options='{ repo = "libusb/hidapi" }'
            )
            + "\n"
            + test_config.spec_entry(
                "local.repo@r0", spec, options='{ repo = "nanopb/nanopb" }'
            )
            + "\n}\n"
        )
        run = self.install(manifest)
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertRegex(run.stderr, r"\[local\.repo@r0\] libusb/hidapi installed")
        self.assertRegex(run.stderr, r"\[local\.repo@r0\] nanopb/nanopb installed")

    def test_display_nil_return_is_the_same_as_absent(self):
        spec = self._spec("n.lua", "local.n@v1", "DISPLAY = function(options) return nil end")
        manifest = self.write_manifest(
            test_config.write_spec_manifest(self.work, [("local.n@v1", spec)]).read_text()
        )
        run = self.install(manifest)
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertRegex(run.stderr, r"\[local\.n@v1\] installed \(\d")

    def test_display_must_be_a_single_line(self):
        """A row is one line; a DISPLAY with a newline would desync the live region."""
        spec = self._spec("m.lua", "local.m@v1", 'DISPLAY = "two\\nlines"')
        manifest = self.write_manifest(
            test_config.write_spec_manifest(self.work, [("local.m@v1", spec)]).read_text()
        )
        run = self.install(manifest)
        self.assertNotEqual(0, run.returncode)
        self.assertIn("DISPLAY must be a single line", run.stderr)

    def test_display_wrong_type_is_an_error(self):
        spec = self._spec("t.lua", "local.t@v1", "DISPLAY = 42")
        manifest = self.write_manifest(
            test_config.write_spec_manifest(self.work, [("local.t@v1", spec)]).read_text()
        )
        run = self.install(manifest)
        self.assertNotEqual(0, run.returncode)
        self.assertIn("DISPLAY must be a string or a function", run.stderr)

    def test_display_function_must_return_a_string(self):
        spec = self._spec("r.lua", "local.r@v1", "DISPLAY = function(options) return 42 end")
        manifest = self.write_manifest(
            test_config.write_spec_manifest(self.work, [("local.r@v1", spec)]).read_text()
        )
        run = self.install(manifest)
        self.assertNotEqual(0, run.returncode)
        self.assertIn("DISPLAY function must return a string or nil", run.stderr)


@unittest.skipIf(sys.platform == "win32", "no pty on Windows")
class TestNoWorkIsSilent(EnvyTestCase):
    def setUp(self):
        super().setUp()
        self.payload = self.work / "payload.txt"
        self.payload.write_text("hello\n", encoding="utf-8")

    def _manifest(self, body: str, identity: str = "local.q@v1") -> Path:
        spec = self.write_spec(
            "q.lua",
            f'IDENTITY = "{identity}"\n'
            f'FETCH = {{ source = "file://{self.lua_path(self.payload)}" }}\n'
            f"{body}\n",
        )
        return self.write_manifest(
            test_config.write_spec_manifest(self.work, [(identity, spec)]).read_text()
        )

    _run_on_pty = TestDisplay._run_on_pty

    def test_warm_cache_run_paints_nothing(self):
        manifest = self._manifest("")

        code, first = self._run_on_pty("install", "--manifest", manifest)
        self.assertEqual(0, code, first)
        self.assertIn("installed", visible(first))

        code, second = self._run_on_pty("install", "--manifest", manifest)
        self.assertEqual(0, code, second)
        self.assertEqual("", visible(second), f"expected a silent run, got: {second!r}")

    def test_off_a_tty_every_package_still_reports(self):
        """The log is a record of the run, so the no-ops stay in it."""
        manifest = self._manifest("")
        self.assertEqual(0, self.install(manifest).returncode)

        run = self.install(manifest)
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertIn("[local.q@v1] cache hit", run.stderr)
        self.assertEqual({"local.q@v1": "cache_hit"}, run.outcomes())

    def test_a_setup_pair_that_ran_keeps_the_row(self):
        """A cache hit is not "no work" when a pair got past its CHECK and installed."""
        spec = self.write_spec(
            "s.lua",
            'IDENTITY = "local.s@v1"\n'
            f'FETCH = {{ source = "file://{self.lua_path(self.payload)}" }}\n'
            "SETUP = { main = {\n"
            "  CHECK = function(pkg_dir, options) return false end,\n"
            "  INSTALL = function(pkg_dir, options) end,\n"
            "} }\n",
        )
        manifest = self.write_manifest(
            "PACKAGES = {\n"
            + test_config.spec_entry("local.s@v1", spec, setup=["main"])
            + "\n}\n"
        )

        code, first = self._run_on_pty("install", "--manifest", manifest)
        self.assertEqual(0, code, first)

        code, second = self._run_on_pty("install", "--manifest", manifest)
        self.assertEqual(0, code, second)
        self.assertIn("[local.s@v1]", visible(second))
        self.assertIn("cache hit", visible(second))
