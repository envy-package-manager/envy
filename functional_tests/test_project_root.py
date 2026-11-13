"""envy.PROJECT_ROOT: the root manifest's directory in every spec state of a run.

Covers manifest scope, spec top level, SETUP pair functions, bundle validation (both the
engine's and envy.loadenv_bundle's), a subproject's manifest (its own directory) against
the specs it declares (still the run's), and its absence outside a project (`envy lua`).
"""

import unittest
from pathlib import Path

from .env import EnvyTestCase

# Records envy.PROJECT_ROOT as each scope saw it. A nil anywhere fails the write.
PROBE_SPEC = """IDENTITY = "local.root_probe@v1"
USER_MANAGED = true
local at_load = envy.PROJECT_ROOT

SETUP = {
  main = {
    CHECK = function() return false end,
    INSTALL = function(_, options)
      local f = assert(io.open(envy.path.join(envy.PROJECT_ROOT, "roots.txt"), "w"))
      f:write(options.from_manifest, "\\n", at_load, "\\n", envy.PROJECT_ROOT, "\\n")
      f:close()
      envy.run("echo x > cwd.txt")  -- relative: lands in SETUP's cwd
    end,
  },
}
"""

# A bundled spec whose top level needs the root: bundle validation executes it too.
BUNDLED_SPEC = """IDENTITY = "test.needs_root@r1"
USER_MANAGED = true
local marker = assert(envy.PROJECT_ROOT, "PROJECT_ROOT unset at spec top level")
SETUP = { main = { CHECK = function() return true end, INSTALL = function() end } }
"""


class TestProjectRoot(EnvyTestCase):
    def setUp(self):
        super().setUp()
        self.project = self.make_temp_dir("project")
        self.elsewhere = self.make_temp_dir("elsewhere")  # cwd: proves it isn't the cwd

    def make_bundle(self) -> Path:
        root = self.make_temp_dir("bundle")
        (root / "envy-bundle.lua").write_text(
            'BUNDLE = "test.rootb@r1"\n'
            'SPECS = { ["test.needs_root@r1"] = "specs/needs_root.lua" }\n',
            encoding="utf-8",
        )
        self.write_spec("specs/needs_root.lua", BUNDLED_SPEC, root)
        self.write_spec("lib/helper.lua", "return { ok = true }\n", root)
        return root

    def bundles_table(self, root: Path) -> str:
        return (
            f'BUNDLES = {{ tools = {{ identity = "test.rootb@r1", '
            f'source = "{self.lua_path(root)}" }} }}\n'
        )

    def test_manifest_spec_and_setup_all_see_the_manifest_dir(self):
        spec = self.write_spec("root_probe.lua", PROBE_SPEC)
        manifest = self.write_manifest(
            f'PACKAGES = {{ {{ spec = "local.root_probe@v1", '
            f'source = "{self.lua_path(spec)}", setup = {{ "main" }}, '
            f"options = {{ from_manifest = envy.PROJECT_ROOT }} }} }}\n",
            directory=self.project,
        )

        run = self.install(manifest, cwd=self.elsewhere)

        self.assertEqual(0, run.returncode, run.stderr)
        roots = (self.project / "roots.txt").read_text(encoding="utf-8").splitlines()
        self.assertEqual(3, len(roots), roots)
        self.assertEqual(1, len(set(roots)), f"scopes disagree: {roots}")
        self.assertEqual(self.project.resolve(), Path(roots[0]))

    def test_a_subproject_manifest_sees_its_own_root_and_its_specs_the_runs(self):
        spec = self.write_spec("root_probe.lua", PROBE_SPEC)
        sub = self.project / "sub"
        sub.mkdir()
        (sub / "envy.lua").write_text(
            f'PACKAGES = {{ {{ spec = "local.root_probe@v1", '
            f'source = "{self.lua_path(spec)}", setup = {{ "main" }}, '
            f"options = {{ from_manifest = envy.PROJECT_ROOT }} }} }}\n",
            encoding="utf-8",
        )
        manifest = self.write_manifest(
            'PACKAGES = envy.subproject("sub").PACKAGES\n', directory=self.project
        )

        run = self.install(manifest, cwd=self.elsewhere)

        self.assertEqual(0, run.returncode, run.stderr)
        roots = (self.project / "roots.txt").read_text(encoding="utf-8").splitlines()
        project = self.project.resolve()
        self.assertEqual([sub.resolve(), project, project], [Path(r) for r in roots])
        self.assertTrue((self.project / "cwd.txt").exists(), "SETUP ran outside the root")

    def test_bundle_validation_sees_it_when_a_package_pulls_the_bundle(self):
        manifest = self.write_manifest(
            self.bundles_table(self.make_bundle())
            + 'PACKAGES = { { spec = "test.needs_root@r1", bundle = "tools" } }\n',
            directory=self.project,
        )

        run = self.install(manifest, cwd=self.elsewhere)

        self.assertEqual(0, run.returncode, run.stderr)

    def test_bundle_validation_sees_it_under_loadenv_bundle(self):
        manifest = self.write_manifest(
            self.bundles_table(self.make_bundle())
            + 'assert(envy.loadenv_bundle("tools", "lib.helper").ok)\n'
            + "PACKAGES = {}\n",
            directory=self.project,
        )

        run = self.install(manifest, cwd=self.elsewhere)

        self.assertEqual(0, run.returncode, run.stderr)

    def test_envy_lua_has_no_project(self):
        script = self.write_spec(
            "probe.lua", 'envy.stdout(tostring(envy.PROJECT_ROOT) .. "\\n")\n'
        )

        run = self.run_envy("lua", script, cwd=self.project)

        self.assertEqual(0, run.returncode, run.stderr)
        self.assertEqual("nil", run.stdout.strip())


if __name__ == "__main__":
    unittest.main()
