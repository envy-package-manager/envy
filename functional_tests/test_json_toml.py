"""envy.json and envy.toml against real files: read, write, and round-trip on disk.

Unit tests cover the codecs on strings; these pin the file half -- relative paths
anchored like envy.copy's, the trailing newline, exact bytes a spec can rely on, and
errors that name the file -- in `envy lua` and inside an INSTALL phase.
"""

from __future__ import annotations

import json
import tomllib
import unittest

from . import test_config
from .env import EnvyTestCase

_DOC = """{
  name = "tool", version = 3, ratio = 0.5, ok = true, tags = { "a", "b" },
  empty = envy.json.array(), deps = { zlib = { version = "1.3" } },
  bin = { { name = "x" }, { name = "y", path = "bin/y" } },
}"""


class TestJsonToml(EnvyTestCase):
    def test_write_then_read_round_trips_exact_bytes(self):
        j, t, e = (self.lua_path(self.work / n) for n in ("o.json", "o.toml", "e.toml"))
        run = self.run_lua(
            f"local doc = {_DOC}\n"
            f'envy.json.write("{j}", doc, {{ indent = 2 }})\n'
            f'envy.toml.write("{t}", doc)\n'
            f'envy.toml.write("{e}", {{}})\n'
            f'local j, t = envy.json.read("{j}"), envy.toml.read("{t}")\n'
            "assert(envy.json.encode(j) == envy.json.encode(doc))\n"
            "assert(envy.toml.encode(t) == envy.toml.encode(doc))\n"
            f'assert(next(envy.toml.read("{e}")) == nil)\n'
        )
        self.assertEqual(0, run.returncode, run.stderr)

        expected = {
            "bin": [{"name": "x"}, {"name": "y", "path": "bin/y"}],
            "deps": {"zlib": {"version": "1.3"}},
            "empty": [],
            "name": "tool",
            "ok": True,
            "ratio": 0.5,
            "tags": ["a", "b"],
            "version": 3,
        }
        json_bytes = (self.work / "o.json").read_bytes()
        self.assertEqual(json.dumps(expected, indent=2).encode() + b"\n", json_bytes)
        toml_text = (self.work / "o.toml").read_text(encoding="utf-8")
        self.assertEqual(
            "\n".join([
                "empty = []", 'name = "tool"', "ok = true", "ratio = 0.5",
                'tags = ["a", "b"]', "version = 3", "",
                "[[bin]]", 'name = "x"', "",
                "[[bin]]", 'name = "y"', 'path = "bin/y"', "",
                "[deps.zlib]", 'version = "1.3"', "",
            ]),
            toml_text,
        )
        self.assertEqual(expected, tomllib.loads(toml_text))
        self.assertEqual(b"", (self.work / "e.toml").read_bytes())

    def test_read_errors_name_the_file(self):
        (self.work / "bad.toml").write_text("a = \n", encoding="utf-8")
        (self.work / "bad.json").write_text('{"a": 01}', encoding="utf-8")
        cases = [
            ("bad.toml", "envy.toml.read", "bad.toml: (line 1) missing value"),
            ("bad.json", "envy.json.read", "bad.json: syntax error at line 1"),
            ("missing.json", "envy.json.read", "cannot read"),
        ]
        for name, fn, message in cases:
            with self.subTest(name=name):
                run = self.run_lua(f'{fn}("{self.lua_path(self.work / name)}")\n')
                self.assertNotEqual(0, run.returncode)
                self.assertIn(f"{fn}: ", run.stderr)
                self.assertIn(message, run.stderr)

    def test_install_phase_writes_and_reads_relative_to_stage(self):
        """Relative paths land in stage_dir, like envy.copy's; install_dir is explicit."""
        spec = self.write_spec(
            "cfg.lua",
            f"""IDENTITY = "local.cfg@r0"
FETCH = function() end
INSTALL = function(install_dir, stage_dir)
  local doc = {_DOC}
  envy.toml.write("scratch/cfg.toml", doc)
  assert(envy.toml.read(envy.path.join(stage_dir, "scratch", "cfg.toml")).name == "tool")
  local back = envy.toml.read("scratch/cfg.toml")
  back.stage = "seen"
  envy.json.write(envy.path.join(install_dir, "cfg.json"), back)
  envy.toml.write(envy.path.join(install_dir, "cfg.toml"), envy.json.read(
    envy.path.join(install_dir, "cfg.json")))
end
""",
        )
        manifest = test_config.write_spec_manifest(self.work, [("local.cfg@r0", spec.name)])
        run = self.install(manifest)
        self.assertEqual(0, run.returncode, run.stderr)

        (entry,) = self.pkg_entries("local.cfg@r0")
        pkg = entry / "pkg"
        written = json.loads((pkg / "cfg.json").read_bytes())
        self.assertEqual("seen", written["stage"])
        self.assertEqual([{"name": "x"}, {"name": "y", "path": "bin/y"}], written["bin"])
        self.assertEqual(written, tomllib.loads((pkg / "cfg.toml").read_text("utf-8")))
        self.assertFalse((pkg / "scratch").exists(), "relative writes belong in stage_dir")


if __name__ == "__main__":
    unittest.main()
