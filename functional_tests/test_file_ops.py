"""Functional tests for `envy.remove`, the Lua verb that deletes a tree.

It deletes through the same native parallel path as the vendor wipe and the cache's
own cleanup -- one syscall per entry over tree_list's walk -- with
std::filesystem::remove_all behind it for whatever does not come away. These pin the
semantics that path has to keep: a symlink is unlinked and never followed, and a
missing path is not an error.
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

from .env import EnvyTestCase

POSIX_ONLY = unittest.skipIf(sys.platform == "win32", "POSIX symlinks")


class TestEnvyRemove(EnvyTestCase):
    def run_lua(self, body: str):
        script = self.work / "ops.lua"
        script.write_text(body, encoding="utf-8")
        return self.run_envy("lua", script)

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


if __name__ == "__main__":
    unittest.main()
