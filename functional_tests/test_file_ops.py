"""Functional tests for envy's Lua file verbs: `envy.remove`, `envy.write`, `envy.mkdir`.

`envy.remove` deletes through the same native parallel path as the vendor wipe and the cache's
own cleanup -- one syscall per entry over tree_list's walk -- with
std::filesystem::remove_all behind it for whatever does not come away. These pin the
semantics that path has to keep: a symlink is unlinked and never followed, and a
missing path is not an error. `envy.write` creates parents, writes bytes untranslated,
and sets the exec bit on request; `envy.mkdir` tolerates an existing directory.
"""

from __future__ import annotations

import os
import stat
import subprocess
import sys
import unittest
from pathlib import Path

from . import test_config
from .env import EnvyTestCase

POSIX_ONLY = unittest.skipIf(sys.platform == "win32", "POSIX symlinks")
POSIX_MODES = unittest.skipIf(sys.platform == "win32", "POSIX file modes")
RUNNING_AS_ROOT = sys.platform != "win32" and os.geteuid() == 0


class TestEnvyRemove(EnvyTestCase):
    def remove(self, target: Path):
        return self.run_lua(f'envy.remove("{self.lua_path(target)}")\n')

    def test_removes_a_directory_tree(self):
        """More files than one work slice, so the parallel pass actually runs."""
        root = self.make_temp_dir("tree")
        for i in range(200):
            directory = root / f"d{i % 7}" / f"e{i % 3}"
            directory.mkdir(parents=True, exist_ok=True)
            (directory / f"f{i}.txt").write_text("x\n", encoding="utf-8")

        run = self.remove(root)
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertFalse(root.exists())

    def test_removes_a_single_file(self):
        target = self.work / "lonely.txt"
        target.write_text("x\n", encoding="utf-8")

        self.assertEqual(0, self.remove(target).returncode)
        self.assertFalse(target.exists())

    def test_removing_a_missing_path_is_not_an_error(self):
        missing = self.work / "never-existed"
        run = self.remove(missing)

        self.assertEqual(0, run.returncode, run.stderr)
        self.assertFalse(missing.exists())

    @POSIX_ONLY
    def test_a_symlinked_directory_is_unlinked_not_followed(self):
        """Following it would delete a tree the caller never named."""
        target = self.make_temp_dir("target")
        (target / "precious.txt").write_text("not yours\n", encoding="utf-8")
        link = self.work / "link"
        link.symlink_to(target)

        run = self.remove(link)
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertFalse(link.is_symlink(), "the link itself should be gone")
        self.assertTrue(
            (target / "precious.txt").exists(),
            "removing a symlink followed it and deleted the target's contents",
        )


class TestEnvyWriteMkdir(EnvyTestCase):
    def test_write_creates_parents_and_keeps_bytes_exact(self):
        """Binary mode: CRLF and NUL survive; a second write replaces the first."""
        target, cmd = self.work / "a" / "b" / "out.bin", self.work / "c" / "tool.cmd"
        empty = self.work / "empty"
        t = self.lua_path(target)
        run = self.run_lua(
            f'envy.write("{t}", "stale", {{ executable = true }})\n'
            f'envy.write("{t}", "x\\r\\ny\\0z\\n", {{ executable = false }})\n'
            f'envy.write("{self.lua_path(cmd)}", "@echo off\\r\\n", {{ executable = true }})\n'
            f'envy.write("{self.lua_path(empty)}", "")\n'
        )
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertEqual(b"x\r\ny\0z\n", target.read_bytes())
        self.assertEqual(b"@echo off\r\n", cmd.read_bytes())  # executable: a no-op on Windows
        self.assertEqual(b"", empty.read_bytes())
        if sys.platform != "win32":
            self.assertEqual(0, target.stat().st_mode & 0o111, "a rewrite keeps no exec bit")

    def test_non_ascii_paths_are_utf8(self):
        """Lua strings are UTF-8; on Windows they must not pass through the ANSI code page."""
        target = self.work / "café" / "naïve.txt"
        run = self.run_lua(
            f'envy.mkdir("{self.lua_path(target.parent)}")\n'
            f'envy.write("{self.lua_path(target)}", "x")\n'
            f'assert(envy.is_file("{self.lua_path(target)}"))\n'
        )
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertEqual(b"x", target.read_bytes())

    @POSIX_MODES  # Windows' MAX_PATH bites first
    def test_a_name_near_name_max_still_writes(self):
        """The temp file's name must not embed the destination's."""
        target = self.work / ("a" * 244)
        run = self.run_lua(f'envy.write("{self.lua_path(target)}", "x")\n')
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertEqual(b"x", target.read_bytes())

    @POSIX_MODES
    def test_a_failed_write_keeps_the_old_file(self):
        """A file-size limit fails the write at flush; the old content must survive."""
        target = self.work / "kept.txt"
        t = self.lua_path(target)
        self.assertEqual(0, self.run_lua(f'envy.write("{t}", "old")\n').returncode)
        script = self.work / "big.lua"
        script.write_text(f'envy.write("{t}", string.rep("x", 3000))\n', encoding="utf-8")
        run = test_config.run(  # ulimit -f is 1–2 KB; SIGXFSZ ignored so write() fails
            ["/bin/sh", "-c", "trap '' XFSZ; ulimit -f 2; exec \"$@\"", "sh",
             self.envy, "--cache-root", self.cache_root, "lua", script],
            capture_output=True,
        )
        self.assertEqual(1, run.returncode, run.stderr)
        self.assertIn("envy.write: cannot write", run.stderr)
        self.assertEqual(b"old", target.read_bytes())
        self.assertEqual([], [p.name for p in self.work.glob(".envy-tmp-*")])

    @POSIX_MODES
    def test_executable_write_runs(self):
        launcher = self.work / "bin" / "hello"
        run = self.run_lua(
            f'envy.write("{self.lua_path(launcher)}", "#!/bin/sh\\necho hi\\n", '
            "{ executable = true })\n"
        )
        self.assertEqual(0, run.returncode, run.stderr)
        mode = launcher.stat().st_mode
        self.assertEqual(stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH, mode & 0o111)
        self.assertTrue(os.access(launcher, os.X_OK))
        out = subprocess.run([str(launcher)], capture_output=True, text=True, check=True)
        self.assertEqual("hi\n", out.stdout)

    def test_mkdir_creates_nested_and_tolerates_existing(self):
        nested = self.work / "x" / "y" / "z"
        n = self.lua_path(nested)
        run = self.run_lua(f'envy.mkdir("{n}")\nenvy.mkdir("{n}")\n')
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertTrue(nested.is_dir())

    def test_errors_are_lua_errors_and_bad_options_write_nothing(self):
        """pcall catches every one, with its cause; a rejected call leaves no file behind."""
        blocker = self.work / "blocker"
        blocker.write_text("file\n", encoding="utf-8")
        out = self.lua_path(self.work / "never.txt")
        b, w = self.lua_path(blocker), self.lua_path(self.work)
        cases = [
            (f'envy.write("{out}", 42)', "envy.write: expected a string, got number"),
            (f'envy.write("{out}", "x", true)', "envy.write: opts must be a table"),
            (f'envy.write("{out}", "x", {{ mode = "0755" }})', "unknown key 'mode'"),
            (f'envy.write("{out}", "x", {{ executable = "yes" }})', "executable must be a"),
            (f'envy.write("{b}/f", "x")', "envy.write: cannot write"),
            (f'envy.write("{w}", "x")', "envy.write: cannot write"),  # a directory is there
            (f'envy.mkdir("{b}")', "envy.mkdir: cannot create"),
            (f'envy.mkdir("{b}/sub")', "envy.mkdir: cannot create"),
            (f'envy.copy("{b}", "{b}/sub/x")', "blocker/sub"),
            (f'envy.move("{b}", "{b}/sub/x")', "blocker/sub"),
        ]
        for call, message in cases:
            with self.subTest(call=call):
                self.assertPathContains(self.lua_error(call), message)
        self.assertFalse((self.work / "never.txt").exists())

    @POSIX_MODES
    @unittest.skipIf(RUNNING_AS_ROOT, "root searches unreadable directories anyway")
    def test_mkdir_names_the_cause_when_the_parent_is_unreadable(self):
        locked = self.make_temp_dir("locked")
        locked.chmod(0)
        self.addCleanup(locked.chmod, 0o755)  # LIFO: runs before make_temp_dir's rmtree
        err = self.lua_error(f'envy.mkdir("{self.lua_path(locked / "sub")}")')
        self.assertIn("envy.mkdir: cannot create", err)
        self.assertIn("Permission denied", err)

    def test_install_phase_resolves_relative_to_stage(self):
        """The issue's launcher: one call, no tmp detour, no chmod process."""
        spec = self.write_spec(
            "tool.lua",
            """IDENTITY = "local.tool@r0"
FETCH = function() end
INSTALL = function(install_dir, stage_dir)
  envy.mkdir("scratch/deep")
  envy.write("scratch/note.txt", "staged")
  assert(envy.is_dir(envy.path.join(stage_dir, "scratch", "deep")))
  assert(envy.is_file(envy.path.join(stage_dir, "scratch", "note.txt")))
  envy.write(envy.path.join(install_dir, "bin", "tool"), "#!/bin/sh\\necho hi\\n",
             { executable = true })
end
""",
        )
        manifest = test_config.write_spec_manifest(self.work, [("local.tool@r0", spec.name)])
        run = self.install(manifest)
        self.assertEqual(0, run.returncode, run.stderr)

        (entry,) = self.pkg_entries("local.tool@r0")
        tool = entry / "pkg" / "bin" / "tool"
        self.assertEqual(b"#!/bin/sh\necho hi\n", tool.read_bytes())
        self.assertFalse((entry / "pkg" / "scratch").exists(), "relative paths are stage's")
        if sys.platform != "win32":
            self.assertTrue(os.access(tool, os.X_OK))


if __name__ == "__main__":
    unittest.main()
