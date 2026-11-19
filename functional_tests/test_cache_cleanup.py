"""A cache entry's scratch dirs come away even when a build left them read-only (#409)."""

from __future__ import annotations

import subprocess
import sys
import unittest

from .env import EnvyTestCase

# Go's module cache, in miniature: a tree whose directories deny write.
_READ_ONLY_TMP = """IDENTITY = "local.ro@r0"
FETCH = function() end
BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir)
  envy.run("d='" .. tmp_dir .. [[modcache'; mkdir -p "$d/x" && touch "$d/x/f" && {lock}]])
end
INSTALL = function(install_dir) envy.run("touch '" .. install_dir .. "done'") end
"""


# An attempt that dies leaving a pkg/ nothing can delete, then the spec gets fixed.
_STALE_PKG_FAILS = """IDENTITY = "local.stale@r0"
FETCH = function() end
BUILD = function(install_dir)
  envy.run("touch '" .. install_dir .. "stale' && chflags uchg '" .. install_dir .. "stale'")
  error("first attempt dies")
end
"""
_STALE_PKG_FIXED = """IDENTITY = "local.stale@r0"
FETCH = function() end
INSTALL = function(install_dir) envy.run("touch '" .. install_dir .. "done'") end
"""


@unittest.skipIf(sys.platform == "win32", "POSIX permission bits")
class TestCacheEntryCleanup(EnvyTestCase):
    def setUp(self):
        super().setUp()
        # A failing run must not strand an undeletable tree in the scratch cache.
        self.addCleanup(self.unlock_cache)

    def unlock_cache(self):
        if sys.platform == "darwin":
            subprocess.run(["chflags", "-R", "nouchg", str(self.cache_root)], check=False)
        subprocess.run(["chmod", "-R", "u+w", str(self.cache_root)], check=False)

    def install_spec(self, lock: str):
        spec = self.write_spec("ro.lua", _READ_ONLY_TMP.format(lock=lock))
        run = self.install(self.write_manifest(
            f'PACKAGES = {{ {{ spec = "local.ro@r0", source = "{self.lua_path(spec)}" }} }}\n'
        ))
        self.assertEqual(0, run.returncode, run.stderr)
        (entry,) = self.pkg_entries("local.ro@r0")
        self.assertTrue((entry / "pkg" / "done").is_file())
        return run, entry

    def test_read_only_directories_are_removed(self):
        run, entry = self.install_spec('chmod -R a-w "$d"')
        self.assertFalse((entry / "work").exists(), sorted((entry / "work").rglob("*")))
        self.assertNotIn("could not remove", run.stderr)

    @unittest.skipUnless(sys.platform == "darwin", "chflags is BSD's")
    def test_what_stays_behind_is_named(self):
        """An immutable file defeats even u+w; the run still succeeds, and says so."""
        run, entry = self.install_spec('chflags uchg "$d/x/f"')
        self.assertTrue((entry / "work").exists())
        self.assertIn("could not remove", run.stderr)
        self.assertPathContains(run.stderr, f"{entry.name}/work")

    @unittest.skipUnless(sys.platform == "darwin", "chflags is BSD's")
    def test_an_unclearable_pkg_dir_fails_the_install(self):
        """Its bytes would ride into the new payload, so envy refuses to build over them."""
        spec = self.write_spec("stale.lua", _STALE_PKG_FAILS)
        manifest = self.write_manifest(
            f'PACKAGES = {{ {{ spec = "local.stale@r0", source = "{self.lua_path(spec)}" }} }}\n'
        )
        self.assertNotEqual(0, self.install(manifest).returncode)

        spec.write_text(_STALE_PKG_FIXED, encoding="utf-8")
        run = self.install(manifest)
        self.assertNotEqual(0, run.returncode, run.stderr)
        self.assertIn("cannot clear", run.stderr)
        (entry,) = self.pkg_entries("local.stale@r0")
        self.assertPathContains(run.stderr, f"{entry.name}/pkg")
        self.assertFalse((entry / "envy-complete").exists())


if __name__ == "__main__":
    unittest.main()
