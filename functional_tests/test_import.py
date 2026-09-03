"""envy.import: a superproject manifest composed from a subproject's.

The imported manifest keeps resolving its own relative paths and bundle aliases, and
learns it was imported through ENVY_IMPORTER. Its header stays inert: discovery never
sees the file, nothing deploys into its bin dir, and the only record that it took part
in the run is the manifest_imported trace event.
"""

import unittest
from pathlib import Path

from . import test_config
from .env import EnvyTestCase

# Pin the superproject to this build: an equal pin is the one value that never re-execs,
# on a dev build or a release, which keeps the version-skew tests about the import.
PINNED_VERSION = test_config.get_envy_version()


def _older_than(version: str) -> str:
    """A version envy orders strictly before `version`.

    A dev build is 0.0.0 with nothing below it, so fall back to a prerelease of the
    same version -- semver sorts that first, which is the skew this needs.
    """
    parts = version.split(".")
    try:
        major, minor, patch = (int(p) for p in parts[:3])
    except ValueError:
        return f"{version}-old"
    if patch:
        return f"{major}.{minor}.{patch - 1}"
    if minor:
        return f"{major}.{minor - 1}.999"
    if major:
        return f"{major - 1}.999.999"
    return f"{version}-old"


OLDER_VERSION = _older_than(PINNED_VERSION)


SUB_SPEC = """IDENTITY = "local.subtool@v1"
PRODUCTS = { subtool = "subtool" }

function FETCH(tmp_dir, options) end

function INSTALL(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  local f = assert(io.open(envy.path.join(install_dir, "subtool"), "w"))
  f:write("payload")
  f:close()
end
"""

# Only reached standalone: the superproject pins its own toolchain instead, which is the
# FI_FWC_CI-style env-var gate that ENVY_IMPORTER replaces.
STANDALONE_SPEC = SUB_SPEC.replace("local.subtool@v1", "local.standalone@v1").replace(
    "subtool", "standalone"
)


class TestImport(EnvyTestCase):
    def setUp(self):
        super().setUp()
        self.tree = self.make_temp_dir("tree").resolve()
        self.sub = self.tree / "sub"
        (self.sub / "specs").mkdir(parents=True)
        (self.sub / "specs" / "subtool.lua").write_text(SUB_SPEC, encoding="utf-8")
        (self.sub / "specs" / "standalone.lua").write_text(
            STANDALONE_SPEC, encoding="utf-8"
        )

    def write_sub(self, version: str | None = None) -> Path:
        """The subproject manifest: relative spec paths, its own bin dir, non-root."""
        pin = f'-- @envy version "{version}"\n' if version else ""
        manifest = self.sub / "envy.lua"
        manifest.write_text(
            '-- @envy bin "sub-bin"\n'
            '-- @envy root "false"\n'
            '-- @envy deploy "true"\n'
            f"{pin}"
            'PACKAGES = {\n'
            '  { spec = "local.subtool@v1", source = "specs/subtool.lua" },\n'
            "}\n"
            "if not ENVY_IMPORTER then\n"
            "  PACKAGES[#PACKAGES + 1] =\n"
            '    { spec = "local.standalone@v1", source = "specs/standalone.lua" }\n'
            "end\n",
            encoding="utf-8",
        )
        return manifest

    def write_super(self, version: str | None = PINNED_VERSION) -> Path:
        pin = f'-- @envy version "{version}"\n' if version else ""
        manifest = self.tree / "envy.lua"
        manifest.write_text(
            '-- @envy bin "envy-bin"\n'
            '-- @envy deploy "true"\n'
            f"{pin}"
            'local sub = envy.import("sub")\n'
            "PACKAGES = sub.PACKAGES\n",
            encoding="utf-8",
        )
        return manifest

    def test_imported_entry_resolves_its_own_relative_source(self):
        self.write_sub()
        run = self.sync(self.write_super())

        self.assertEqual(0, run.returncode, run.stderr)
        self.assertIn("local.subtool@v1", run.outcomes())
        self.assertTrue(self.pkg_complete("local.subtool@v1"))

    def test_envy_importer_gates_the_standalone_only_entry(self):
        self.write_sub()
        run = self.sync(self.write_super())

        self.assertEqual(0, run.returncode, run.stderr)
        self.assertNotIn("local.standalone@v1", run.outcomes())

    def test_the_same_manifest_run_on_its_own_takes_the_standalone_branch(self):
        sub = self.write_sub()
        run = self.sync(sub)  # named outright: the walk would find the superproject

        self.assertEqual(0, run.returncode, run.stderr)
        self.assertIn("local.standalone@v1", run.outcomes())

    def test_manifest_imported_names_the_file_and_its_importer(self):
        sub = self.write_sub()
        super_manifest = self.write_super()
        run = self.sync(super_manifest)

        self.assertEqual(0, run.returncode, run.stderr)
        events = run.events("manifest_imported")
        self.assertEqual(1, len(events), f"expected one import: {events}")
        self.assertPathEndsWith(events[0].raw["path"], "sub/envy.lua")
        self.assertPathEndsWith(events[0].raw["importer"], super_manifest.name)

    def test_discovery_never_resolves_the_imported_manifest(self):
        self.write_sub()
        run = self.sync(self.write_super())

        self.assertEqual(0, run.returncode, run.stderr)
        resolved = [e.raw["path"] for e in run.events("manifest_resolved")]
        self.assertTrue(resolved, "no manifest_resolved event at all")
        for path in resolved:
            self.assertNotIn("sub", Path(path).parent.name, f"discovery saw {path}")

    def test_deploy_writes_into_the_superproject_bin_only(self):
        self.write_sub()
        run = self.sync(self.write_super())

        self.assertEqual(0, run.returncode, run.stderr)
        deployed = [e.raw["product"] for e in run.events("deploy_script")]
        self.assertIn("subtool", deployed, f"products deployed: {deployed}")

        scripts = list((self.tree / "envy-bin").glob("subtool*"))
        self.assertTrue(scripts, "no product script in the superproject bin dir")
        self.assertFalse(
            list((self.sub / "sub-bin").glob("subtool*")),
            "the imported manifest's bin dir must stay inert",
        )

    def test_imported_manifest_requiring_a_newer_envy_is_an_error(self):
        self.write_sub(version="99.0.0")
        run = self.sync(self.write_super())

        self.assertNotEqual(0, run.returncode, run.stdout)
        self.assertIn("requires envy 99.0.0", run.stderr)

    def test_imported_manifest_pinning_an_older_envy_only_warns(self):
        self.write_sub(version=OLDER_VERSION)
        run = self.sync(self.write_super())

        self.assertEqual(0, run.returncode, run.stderr)
        self.assertIn(f"pins envy {OLDER_VERSION}", run.stderr)

    def test_a_missing_import_names_the_argument_and_the_resolved_path(self):
        manifest = self.tree / "envy.lua"
        manifest.write_text(
            '-- @envy bin "envy-bin"\nPACKAGES = envy.import("nope").PACKAGES\n',
            encoding="utf-8",
        )
        run = self.sync(manifest)

        self.assertNotEqual(0, run.returncode, run.stdout)
        self.assertIn("'nope' not found (resolved to ", run.stderr)


if __name__ == "__main__":
    unittest.main()
