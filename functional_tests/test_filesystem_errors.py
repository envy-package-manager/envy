"""Filesystem failures reach the user as errors that say what failed, never as aborts.

libc++ throws std::filesystem's errors from inside its dylib, which macOS code built with
-fno-rtti can't catch as std::exception: pcall saw "caught (...) exception", and a deleted
working directory or an unreadable one aborted envy. envy builds with RTTI; these pin the
real paths (POSIX-only where Windows can't stage the failure), test_exception_boundaries
every boundary. Assertions name paths and errno text, not one C++ library's wording.
"""

from __future__ import annotations

import os
import sys
import unittest

from . import test_config
from .env import EnvyTestCase

POSIX_ONLY = unittest.skipIf(sys.platform == "win32", "POSIX cwd, TMPDIR and file modes")
RUNNING_AS_ROOT = sys.platform != "win32" and os.geteuid() == 0


class TestFilesystemErrorsReachTheUser(EnvyTestCase):
    def run_lua(self, body: str, **kwargs):
        script = self.work / "fs.lua"
        script.write_text(body, encoding="utf-8")
        return self.run_envy("lua", script, **kwargs)

    def test_copy_and_move_name_the_blocked_directory(self):
        blocker = self.work / "blocker"
        blocker.write_text("file\n", encoding="utf-8")
        b = self.lua_path(blocker)
        for fn in ("envy.copy", "envy.move"):
            with self.subTest(fn=fn):
                run = self.run_lua(
                    f'local ok, err = pcall({fn}, "{b}", "{b}/sub/x")\n'
                    'assert(not ok, "expected an error")\n'
                    "io.stderr:write(err)\n"
                )
                self.assertEqual(0, run.returncode, run.stderr)
                self.assertPathContains(run.stderr, "blocker/sub")

    @POSIX_ONLY
    def test_a_deleted_working_directory_is_an_error_not_an_abort(self):
        script = self.work / "rel.lua"
        script.write_text('envy.exists("relative")\n', encoding="utf-8")
        for args in (["lua", script], ["install"]):
            with self.subTest(command=args[0]):
                gone = self.make_temp_dir("gone")
                run = test_config.run(
                    ["/bin/sh", "-c", 'cd "$1" && rmdir "$1" && shift && exec "$@"', "sh",
                     gone, self.envy, "--cache-root", self.cache_root, *args],
                    capture_output=True,
                )
                self.assertEqual(1, run.returncode, test_config.describe_exit(run))
                self.assertRegex(run.stderr, r"current.path")
                self.assertIn("No such file or directory", run.stderr)

    @POSIX_ONLY
    def test_an_unusable_temp_directory_is_named(self):
        missing = self.work / "missing"
        run = self.run_lua('envy.run("echo hi")\n', env={**os.environ, "TMPDIR": str(missing)})
        self.assertEqual(1, run.returncode, run.stderr)
        self.assertIn(str(missing), run.stderr)

    @POSIX_ONLY
    @unittest.skipIf(RUNNING_AS_ROOT, "root lists unreadable directories anyway")
    def test_an_unreadable_directory_is_named(self):
        locked = self.make_temp_dir("locked")
        locked.chmod(0)
        self.addCleanup(locked.chmod, 0o755)  # LIFO: runs before make_temp_dir's rmtree
        run = self.run_envy("hash", locked)
        self.assertEqual(1, run.returncode, run.stderr)
        self.assertIn(str(locked), run.stderr)
        self.assertIn("Permission denied", run.stderr)


if __name__ == "__main__":
    unittest.main()
