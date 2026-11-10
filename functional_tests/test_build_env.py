"""A spec's ENV reaches the phases of packages that depend on it, transitively.

A's INSTALL runs B's tool, and B's tool runs C's cargo: C's CARGO_HOME has to be in
A's environment, sandboxed under A's own tmp dir, never the user's and never a project's.
Activation (an `envy activate`d shell) must never reach a phase at all.
"""

from __future__ import annotations

import os
import shutil
import sys
import unittest
from pathlib import Path

from . import test_config
from .env import EnvyTestCase

_C = """IDENTITY = "local.c@r0"
FETCH = function() end
INSTALL = function() end
ENV = function(pkg_path, root, options)
  return { CARGO_HOME = envy.path.join(root, ".cargo"), DEPTH = "c", ONLY_C = "c" }
end
"""

# B's tool reports what it was run with; B's ENV puts it on PATH for its dependents.
_B = """IDENTITY = "local.b@r0"
DEPENDENCIES = { { spec = "local.c@r0", source = "c.lua" } }
FETCH = function() end
INSTALL = function(install_dir)
  envy.run([[mkdir -p bin && cat > bin/btool <<'TOOL'
#!/bin/sh
echo "CARGO_HOME=$CARGO_HOME"
echo "DEPTH=$DEPTH"
echo "ONLY_C=$ONLY_C"
echo "FOO=${FOO-none}"
echo "ACTIVE=${ENVY_ACTIVATE_ROOT-none}"
TOOL
chmod +x bin/btool]], { cwd = install_dir })
end
ENV = function(pkg_path)
  return { DEPTH = "b", PATH = { prepend = envy.path.join(pkg_path, "bin") } }
end
"""

_A_FUNCTION = """IDENTITY = "local.a@r0"
DEPENDENCIES = { { spec = "local.b@r0", source = "b.lua" } }
FETCH = function() end
INSTALL = function(install_dir)
  envy.run('btool > "$ENVY_TEST_OUT"')
end
"""

_A_STRING = """IDENTITY = "local.a@r0"
DEPENDENCIES = { { spec = "local.b@r0", source = "b.lua" } }
FETCH = function() end
INSTALL = 'btool > "$ENVY_TEST_OUT"'
"""


def _report(path: Path) -> dict[str, str]:
    return dict(line.split("=", 1) for line in path.read_text().splitlines() if "=" in line)


@unittest.skipIf(sys.platform == "win32", "the fixture tool is a POSIX shell script")
class TestBuildEnv(EnvyTestCase):
    envy_watchdog_timeout = 30

    def _project(self, a_spec: str) -> Path:
        self.write_spec("c.lua", _C)
        self.write_spec("b.lua", _B)
        self.write_spec("a.lua", a_spec)
        return test_config.write_spec_manifest(self.work, [("local.a@r0", "a.lua")])

    def _install(self, manifest: Path, **env_overrides: str) -> dict[str, str]:
        out = self.work / "report.txt"
        env = test_config.get_test_env()
        env.update(env_overrides)
        env["ENVY_TEST_OUT"] = str(out)
        run = self.install(manifest, env=env)
        self.assertEqual(0, run.returncode, run.stderr)
        return _report(out)

    def test_transitive_env_reaches_install_and_nearer_wins(self) -> None:
        report = self._install(self._project(_A_FUNCTION))
        self.assertEqual("b", report["DEPTH"])  # B is nearer to A than C is
        self.assertEqual("c", report["ONLY_C"])  # C reaches A through B
        cargo_home = Path(report["CARGO_HOME"])
        self.assertEqual(".cargo", cargo_home.name)
        self.assertIn("local.a@r0", cargo_home.as_posix())  # A's own sandbox

    def test_string_verb_gets_the_same_env(self) -> None:
        report = self._install(self._project(_A_STRING))
        self.assertEqual("b", report["DEPTH"])
        self.assertIn("local.a@r0", report["CARGO_HOME"])

    def test_dependency_cache_hit_still_contributes(self) -> None:
        manifest = self._project(_A_FUNCTION)
        self._install(manifest)
        for entry in self.pkg_entries("local.a@r0"):
            shutil.rmtree(entry)
        report = self._install(manifest)  # B and C are cache hits now
        self.assertEqual("c", report["ONLY_C"])

    def test_activation_never_reaches_a_phase(self) -> None:
        # What an activated shell looks like: FOO set by activation, originally unset.
        report = self._install(
            self._project(_A_FUNCTION),
            FOO="activated",
            ENVY_ACTIVATE_SCHEMA="1",
            ENVY_ACTIVATE_VARS="FOO",
            ENVY_ACTIVATE_ROOT="/some/project",
        )
        self.assertEqual("none", report["FOO"])
        self.assertEqual("none", report["ACTIVE"])

    def test_setup_pair_sees_dependency_env(self) -> None:
        self.write_spec("c.lua", _C)
        self.write_spec(
            "u.lua",
            """IDENTITY = "local.u@r0"
USER_MANAGED = true
DEPENDENCIES = { { spec = "local.c@r0", source = "c.lua" } }
SETUP = {
  main = {
    CHECK = 'echo "ONLY_C=$ONLY_C" > "$ENVY_TEST_OUT"; exit 1',
    INSTALL = 'echo "CARGO_HOME=$CARGO_HOME" >> "$ENVY_TEST_OUT"',
  },
}
""",
        )
        manifest = test_config.write_spec_manifest(
            self.work, [test_config.spec_entry("local.u@r0", "u.lua", setup=["main"])]
        )
        report = self._install(manifest)
        self.assertEqual("c", report["ONLY_C"])
        self.assertTrue(report["CARGO_HOME"].endswith(".cargo"))

    def test_malformed_env_fails_the_spec(self) -> None:
        self.write_spec("bad.lua", 'IDENTITY = "local.bad@r0"\nFETCH = function() end\nENV = 3\n')
        manifest = test_config.write_spec_manifest(self.work, [("local.bad@r0", "bad.lua")])
        run = self.install(manifest)
        self.assertNotEqual(0, run.returncode)
        self.assertIn("ENV must be a table or a function", run.stderr)

    def test_reserved_env_name_fails_the_spec(self) -> None:
        self.write_spec(
            "bad.lua",
            'IDENTITY = "local.bad@r0"\nFETCH = function() end\nENV = { ENVY_CACHE_ROOT = "/x" }\n',
        )
        manifest = test_config.write_spec_manifest(self.work, [("local.bad@r0", "bad.lua")])
        run = self.install(manifest)
        self.assertNotEqual(0, run.returncode)
        self.assertIn("reserved", run.stderr)


    def test_os_execute_and_io_popen_are_refused(self) -> None:
        # Both spawn with the live (possibly activated) environment, so a spec must use
        # envy.run, whose environment is the phase's.
        for call in ('os.execute("true")', 'io.popen("true"):close()'):
            with self.subTest(call=call):
                self.write_spec(
                    "x.lua",
                    'IDENTITY = "local.x@r0"\nFETCH = function() end\n'
                    f"INSTALL = function() {call} end\n",
                )
                manifest = test_config.write_spec_manifest(self.work, [("local.x@r0", "x.lua")])
                run = self.install(manifest)
                self.assertNotEqual(0, run.returncode)
                self.assertIn("use envy.run", run.stderr)

    def test_manifest_bundle_fetch_sees_its_dependencies_env(self) -> None:
        self.write_spec("c.lua", _C)
        bundle = self.work / "bundle"
        (bundle / "specs").mkdir(parents=True)
        (bundle / "envy-bundle.lua").write_text(
            'BUNDLE = "test.b@v1"\nSPECS = { ["test.s@v1"] = "specs/s.lua" }\n'
        )
        (bundle / "specs" / "s.lua").write_text(
            'IDENTITY = "test.s@v1"\nFETCH = function() end\nINSTALL = function() end\n'
        )
        out = self.work / "report.txt"
        manifest = self.work / "envy.lua"
        manifest.write_text(
            test_config.make_manifest(
                f"""BUNDLES = {{
  b = {{
    identity = "test.b@v1",
    source = {{
      fetch = function(tmp_dir)
        envy.run('echo "ONLY_C=$ONLY_C" > "{out.as_posix()}"')
        envy.run("cp -r '{bundle.as_posix()}/'* " .. tmp_dir .. "/")
        envy.commit_fetch({{ "envy-bundle.lua", "specs" }})
      end,
      dependencies = {{ {{ spec = "local.c@r0", source = "c.lua" }} }},
    }},
  }},
}}
PACKAGES = {{ {{ spec = "test.s@v1", bundle = "b" }} }}
"""
            )
        )
        run = self.install(manifest)
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertEqual("c", _report(out)["ONLY_C"])

if __name__ == "__main__":
    unittest.main()
