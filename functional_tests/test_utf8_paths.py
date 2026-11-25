"""Non-ASCII project and cache paths round-trip through every kind of Lua.

envy.* reads Lua strings as UTF-8; plain Lua (io.open, dofile) goes through the C
runtime's narrow fopen, which reads the process code page. On Windows the two agree only
because src/resources/windows_utf8.manifest makes that code page UTF-8; elsewhere narrow
strings are UTF-8 already, so this test pins Windows.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

from . import test_config
from .env import EnvyTestCase

_NAME = "café-日本"  # é is in cp1252; 日本 is in no Western code page

_SPEC = """IDENTITY = "local.uni@v1"
FETCH = function() end
INSTALL = function(install_dir)
  local f = assert(io.open(install_dir .. "lua.txt", "w"))  -- the C runtime's fopen
  f:write(dofile(envy.PROJECT_ROOT .. "/helper.lua"))       -- a project path, too
  f:close()
  envy.write(install_dir .. "envy.txt", "envy")             -- envy's UTF-8 reader
end
PRODUCTS = { show = { command = function(pkg_dir)
  return { PY, "-c", "import sys;print(open(sys.argv[1]).read())", pkg_dir .. "envy.txt" }
end } }
"""


class TestUtf8Paths(EnvyTestCase):
    use_production_binary = True  # the shipped envy.exe must carry the manifest
    def test_non_ascii_project_and_cache_round_trip_through_lua(self):
        self.cache_root = self.make_temp_dir("cache") / _NAME
        project = self.work / _NAME
        project.mkdir()
        (project / "helper.lua").write_text('return "helper"\n', encoding="utf-8")
        spec = self.write_spec(
            "uni.lua",
            _SPEC.replace("PY", json.dumps(Path(sys.executable).as_posix())),
            directory=project)
        m = self.write_manifest(
            "PACKAGES = {\n" + test_config.spec_entry("local.uni@v1", spec) + "\n}\n",
            directory=project)

        r = self.install(m)
        self.assertEqual(r.returncode, 0, r.stderr)
        r = self.run_envy("package", "local.uni@v1", "--manifest", m)
        self.assertEqual(r.returncode, 0, r.stderr)
        pkg = Path(r.stdout.strip())
        self.assertIn(_NAME, str(pkg))  # under the cache root, spelled as Python spells it
        self.assertEqual((pkg / "lua.txt").read_text(encoding="utf-8"), "helper")
        self.assertEqual((pkg / "envy.txt").read_text(encoding="utf-8"), "envy")

        r = self.run_envy("product", "--manifest", m, "--run", "show")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout.strip(), "envy")


if __name__ == "__main__":
    import unittest

    unittest.main()
