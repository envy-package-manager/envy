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
import select
import subprocess
import sys
import time
import unittest
from pathlib import Path

from . import test_config
from .env import EnvyTestCase
from .test_vendor import VendorTestCase, rmtree_retry

ANSI = re.compile(r"\x1b\[([0-9;?]*)([A-Za-z])")


def visible(painted: str) -> str:
    """Every character envy painted, escapes stripped -- including ones it later erased."""
    return ANSI.sub("", painted).replace("\r", "").strip()


def screen(painted: str) -> str:
    """What a terminal would still be showing once `painted` has been replayed.

    The live region redraws in place and erases what it shrinks past, so the bytes envy
    wrote are not what a person is left looking at: a step that finishes between two
    render cycles paints a frame and takes it back. Only a screen model can tell that
    apart from a row left on screen, which is the thing these tests are about.
    """
    rows, row, col = [""], 0, 0

    def touch(n):
        while len(rows) <= n:
            rows.append("")

    i = 0
    while i < len(painted):
        ch = painted[i]
        if ch == "\x1b":
            if not (m := ANSI.match(painted, i)):
                i += 1
                continue
            params, final = m.group(1), m.group(2)
            if final == "F":  # cursor previous line: up N, column 1
                row = max(0, row - int(params or 1))
                col = 0
            elif final == "K":  # erase to end of line
                rows[row] = rows[row][:col]
            elif final == "J":  # erase to end of screen
                rows[row] = rows[row][:col]
                del rows[row + 1 :]
            i = m.end()  # every other sequence is a mode toggle, which shows nothing
            continue
        if ch == "\r":
            col = 0
        elif ch == "\n":
            row += 1
            col = 0
            touch(row)
        else:
            line = rows[row].ljust(col)
            rows[row] = line[:col] + ch + line[col + 1 :]
            col += 1
        i += 1

    return "\n".join(rows).strip()


# Bounds the whole pty run. The suite's own watchdog would otherwise be the only limit,
# and it kills the runner rather than failing the test that hung.
PTY_TIMEOUT_S = 30.0


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

        deadline = time.monotonic() + PTY_TIMEOUT_S
        chunks: list[bytes] = []
        overran = False
        try:
            while True:
                remaining = deadline - time.monotonic()
                # os.read on a pty blocks forever if the child wedges without writing, so
                # every wait is bounded and the child is killed rather than waited out.
                if remaining <= 0 or not select.select([primary], [], [], remaining)[0]:
                    overran = True
                    break
                try:
                    data = os.read(primary, 65536)
                except OSError:  # the last writer closed the pty
                    break
                if not data:
                    break
                chunks.append(data)
        finally:
            os.close(primary)

        painted = b"".join(chunks).decode("utf-8", "replace")
        if overran:
            proc.kill()
            proc.wait()
            self.fail(f"envy {args} ran past {PTY_TIMEOUT_S}s; painted: {painted!r}")

        try:
            proc.wait(timeout=max(deadline - time.monotonic(), 1.0))
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
            self.fail(f"envy {args} closed its pty but never exited")
        return proc.returncode, painted

    # -- DISPLAY -------------------------------------------------------------

    def test_display_string_leads_column_two(self):
        """A literal DISPLAY prints after the identity column, before the outcome."""
        spec = self._spec("d.lua", "local.d@v1", 'DISPLAY = "the payload"')
        manifest = test_config.write_spec_manifest(self.work, [("local.d@v1", spec)])
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
        manifest = test_config.write_spec_manifest(self.work, [("local.n@v1", spec)])
        run = self.install(manifest)
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertRegex(run.stderr, r"\[local\.n@v1\] installed \(\d")

    def test_display_must_be_a_single_line(self):
        """A row is one line; a DISPLAY with a newline would desync the live region."""
        spec = self._spec("m.lua", "local.m@v1", 'DISPLAY = "two\\nlines"')
        manifest = test_config.write_spec_manifest(self.work, [("local.m@v1", spec)])
        run = self.install(manifest)
        self.assertNotEqual(0, run.returncode)
        self.assertIn("DISPLAY must be a single line", run.stderr)

    def test_display_rejects_control_characters(self):
        """A NUL truncates the row at the %s that writes it; an ESC steers the cursor."""
        cases = (("nul", '"a\\0b"'), ("esc", '"a\\27[2Jb"'), ("tab", '"a\\9b"'))
        # Numbered directories, not named ones: `nul` is a reserved device on Windows, so
        # mkdir("nul") leaves nothing to write a manifest into.
        for i, (name, literal) in enumerate(cases):
            with self.subTest(name):
                spec = self._spec(f"c{name}.lua", f"local.c{name}@v1",
                                  f"DISPLAY = {literal}")
                where = self.work / f"ctl{i}"
                where.mkdir(exist_ok=True)
                manifest = test_config.write_spec_manifest(
                    where, [(f"local.c{name}@v1", spec)]
                )
                run = self.install(manifest)
                self.assertNotEqual(0, run.returncode, run.stderr)
                self.assertIn("single line of printable text", run.stderr)

    @unittest.skipIf(sys.platform == "win32", "no pty on Windows")
    def test_display_pads_by_columns_not_bytes(self):
        """`café` is five bytes but four columns, so padding by size() misaligns it.

        Both identities are the same length, so the label column cancels out and the only
        thing that can move `installed` is how the display column was measured.
        """
        wide = self._spec("w.lua", "local.w@v1", 'DISPLAY = "café"')
        plain = self._spec("p.lua", "local.p@v1", 'DISPLAY = "abcde"')
        manifest = self.write_manifest(
            "PACKAGES = {\n"
            + test_config.spec_entry("local.w@v1", wide)
            + "\n"
            + test_config.spec_entry("local.p@v1", plain)
            + "\n}\n"
        )
        code, out = self._run_on_pty("install", "--manifest", manifest)
        self.assertEqual(0, code, out)

        rows = {ln.split("]")[0] + "]": ln for ln in screen(out).splitlines()}
        self.assertEqual(
            rows["[local.w@v1]"].index("installed"),
            rows["[local.p@v1]"].index("installed"),
            rows,
        )

    def test_display_wrong_type_is_an_error(self):
        spec = self._spec("t.lua", "local.t@v1", "DISPLAY = 42")
        manifest = test_config.write_spec_manifest(self.work, [("local.t@v1", spec)])
        run = self.install(manifest)
        self.assertNotEqual(0, run.returncode)
        self.assertIn("DISPLAY must be a string or a function", run.stderr)

    def test_display_function_must_return_a_string(self):
        spec = self._spec("r.lua", "local.r@v1", "DISPLAY = function(options) return 42 end")
        manifest = test_config.write_spec_manifest(self.work, [("local.r@v1", spec)])
        run = self.install(manifest)
        self.assertNotEqual(0, run.returncode)
        self.assertIn("DISPLAY function must return a string or nil", run.stderr)


@unittest.skipIf(sys.platform == "win32", "no pty on Windows")
class TestNoWorkIsSilent(EnvyTestCase):
    envy_watchdog_timeout = 60

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
        return test_config.write_spec_manifest(self.work, [(identity, spec)])

    _run_on_pty = TestDisplay._run_on_pty

    def test_warm_cache_run_paints_nothing(self):
        manifest = self._manifest("")

        code, first = self._run_on_pty("install", "--manifest", manifest)
        self.assertEqual(0, code, first)
        self.assertIn("installed", screen(first))

        code, second = self._run_on_pty("install", "--manifest", manifest)
        self.assertEqual(0, code, second)
        self.assertEqual("", screen(second), f"expected a silent run, got: {second!r}")

    def test_a_run_with_nothing_to_say_writes_no_bytes(self):
        """Not "nothing is left on screen" -- nothing was ever sent.

        `screen()` cannot see a frame that was painted and taken back, and neither can a
        mode toggle: hiding the cursor for a run that never draws a row blinks it once
        for no reason. The only assertion that covers both is the byte count.
        """
        manifest = self._manifest("")
        code, first = self._run_on_pty("install", "--manifest", manifest)
        self.assertEqual(0, code, first)

        code, second = self._run_on_pty("install", "--manifest", manifest)
        self.assertEqual(0, code, second)
        self.assertEqual("", second, f"a no-work run touched the terminal: {second!r}")

    def test_no_display_anywhere_means_no_second_column(self):
        """The column exists only if some spec asked for it; nobody pays for dead space."""
        short = self.write_spec(
            "s.lua",
            'IDENTITY = "local.s@v1"\n'
            f'FETCH = {{ source = "file://{self.lua_path(self.payload)}" }}\n',
        )
        longer = self.write_spec(
            "l.lua",
            'IDENTITY = "local.a-much-longer-name@v1"\n'
            f'FETCH = {{ source = "file://{self.lua_path(self.payload)}" }}\n',
        )
        manifest = test_config.write_spec_manifest(
            self.work, [("local.s@v1", short), ("local.a-much-longer-name@v1", longer)]
        )

        code, out = self._run_on_pty("install", "--manifest", manifest)
        self.assertEqual(0, code, out)

        rows = {ln.split("]")[0] + "]": ln for ln in screen(out).splitlines()}
        # The widest label sets the column, so its own row has exactly one space after it:
        # anything more is a display column nobody asked for.
        widest = rows["[local.a-much-longer-name@v1]"]
        self.assertTrue(
            widest.startswith("[local.a-much-longer-name@v1] installed"), widest
        )

    def test_a_silent_package_s_display_does_not_pad_the_row_that_drew(self):
        """A warm-cache package draws nothing and is deleted, so it owns no column."""
        loud = self.write_spec(
            "loud.lua",
            'IDENTITY = "local.loud@v1"\n'
            f'FETCH = {{ source = "file://{self.lua_path(self.payload)}" }}\n'
            'DISPLAY = "a-very-long-display-string-indeed"\n',
        )
        quiet = self.write_spec(
            "quiet.lua",
            'IDENTITY = "local.quiet@v1"\n'
            f'FETCH = {{ source = "file://{self.lua_path(self.payload)}" }}\n',
        )

        # Warm `loud` on its own, then add `quiet`: only `quiet` has work left to do.
        alone = self.work / "alone"
        alone.mkdir(exist_ok=True)
        first = test_config.write_spec_manifest(alone, [("local.loud@v1", loud)])
        self.assertEqual(0, self.install(first).returncode)

        both = self.work / "both"
        both.mkdir(exist_ok=True)
        manifest = test_config.write_spec_manifest(
            both, [("local.loud@v1", loud), ("local.quiet@v1", quiet)]
        )
        code, out = self._run_on_pty("install", "--manifest", manifest)
        self.assertEqual(0, code, out)

        painted = screen(out)
        self.assertNotIn("a-very-long-display-string-indeed", painted)
        self.assertEqual("[local.quiet@v1] installed", painted.split(" (")[0], painted)

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
        self.assertIn("[local.s@v1]", screen(second))
        self.assertIn("cache hit", screen(second))


@unittest.skipIf(sys.platform == "win32", "no pty on Windows")
class TestVendorRows(VendorTestCase):
    """Vendoring is work a cache hit can still have done -- but only when it writes."""

    envy_watchdog_timeout = 60
    _run_on_pty = TestDisplay._run_on_pty

    def setUp(self):
        super().setUp()
        self.dest = self.project / "vendor" / "cobs"

    def _install_once(self, auto_sync: str) -> Path:
        manifest = self.manifest(
            self.entry(
                "local.cobs@r1",
                self.spec("local.cobs@r1"),
                vendor=f'{{ path = "vendor/cobs", auto_sync = {auto_sync} }}',
            )
        )
        self.assertEqual(0, self.install(manifest).returncode)
        return manifest

    def test_a_vendor_copy_reports_the_copy_not_the_cache_hit(self):
        """"cache hit" is the payload's verdict; the row is about what vendoring wrote."""
        manifest = self._install_once("true")
        rmtree_retry(self.dest)  # cached payload, absent destination: the copy is the work

        code, out = self._run_on_pty("install", "--manifest", manifest)
        self.assertEqual(0, code, out)
        self.assertIn("vendored 4 files", screen(out))
        self.assertNotIn("cache hit", screen(out))

    def test_an_exempt_mismatch_reports_but_draws_no_row(self):
        """`auto_sync = false` writes nothing, so the warning is the whole report."""
        manifest = self._install_once("false")
        (self.dest / "src" / "lib.c").write_text("mine now\n", encoding="utf-8")

        code, out = self._run_on_pty("install", "--manifest", manifest)
        self.assertEqual(0, code, out)
        # The warning is scrollback and stays up; what must not follow it is a row.
        painted = screen(out)
        self.assertIn("no longer matches", painted)
        self.assertEqual(1, len(painted.splitlines()), painted)

    def test_an_up_to_date_vendor_copy_is_silent(self):
        manifest = self._install_once("true")

        code, out = self._run_on_pty("install", "--manifest", manifest)
        self.assertEqual(0, code, out)
        self.assertEqual("", screen(out), f"expected a silent run, got: {out!r}")

    def test_an_up_to_date_vendor_copy_paints_no_frame_to_take_back(self):
        """The phase hashes before it knows there is nothing to say.

        Both hashes finish in microseconds on this payload, so the spinner they raise is
        a row that appears and is deleted -- invisible to `screen()`, a flicker to a
        person. Nothing the phase decided is worth a byte here.
        """
        manifest = self._install_once("true")

        code, out = self._run_on_pty("install", "--manifest", manifest)
        self.assertEqual(0, code, out)
        self.assertEqual("", out, f"a no-op vendor phase painted: {out!r}")

    def test_an_exempt_mismatch_paints_the_warning_and_nothing_else(self):
        """`auto_sync = false` writes nothing, so the warning is the entire run.

        Scrollback needs no escapes at all: one of them means a live region opened, and
        the only row it could have held is one the phase decided not to earn.
        """
        manifest = self._install_once("false")
        (self.dest / "src" / "lib.c").write_text("mine now\n", encoding="utf-8")

        code, out = self._run_on_pty("install", "--manifest", manifest)
        self.assertEqual(0, code, out)
        self.assertIn("no longer matches", out)
        self.assertNotIn("\x1b", out, f"a live region opened for a run with no row: {out!r}")

    def test_envy_vendor_says_where_it_copied_to_once(self):
        """`envy vendor` prints its own report, so the row must not say it a second time."""
        manifest = self._install_once("true")
        rmtree_retry(self.dest)

        run = self.run_envy("vendor", "--all", "--manifest", manifest)
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertEqual(1, run.stderr.count(self.shown(self.dest)), run.stderr)
