"""envy.subproject: a superproject manifest composed from a subproject's.

The subproject keeps resolving its own relative paths and bundle aliases, sees its own
directory as envy.PROJECT_ROOT, and learns it is one through envy.IS_SUBPROJECT. Its
header stays inert: discovery never sees the file, nothing deploys into its bin dir, and
the only record that it took part in the run is the subproject_loaded trace event.
"""

import unittest
from pathlib import Path

from . import test_config
from .env import EnvyTestCase

# Pin the superproject to this build: an equal pin is the one value that never re-execs,
# on a dev build or a release, which keeps the version-skew tests about the subproject.
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
# FI_FWC_CI-style env-var gate that envy.IS_SUBPROJECT replaces.
STANDALONE_SPEC = SUB_SPEC.replace("local.subtool@v1", "local.standalone@v1").replace(
    "subtool", "standalone"
)


class TestSubproject(EnvyTestCase):
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
            "if not envy.IS_SUBPROJECT then\n"
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
            'local sub = envy.subproject("sub")\n'
            "PACKAGES = sub.PACKAGES\n",
            encoding="utf-8",
        )
        return manifest

    def test_a_subproject_entry_resolves_its_own_relative_source(self):
        self.write_sub()
        run = self.sync(self.write_super())

        self.assertEqual(0, run.returncode, run.stderr)
        self.assertIn("local.subtool@v1", run.outcomes())
        self.assertTrue(self.pkg_complete("local.subtool@v1"))

    def test_is_subproject_gates_the_standalone_only_entry(self):
        self.write_sub()
        run = self.sync(self.write_super())

        self.assertEqual(0, run.returncode, run.stderr)
        self.assertNotIn("local.standalone@v1", run.outcomes())

    def test_the_same_manifest_run_on_its_own_takes_the_standalone_branch(self):
        sub = self.write_sub()
        run = self.sync(sub)  # named outright: the walk would find the superproject

        self.assertEqual(0, run.returncode, run.stderr)
        self.assertIn("local.standalone@v1", run.outcomes())

    def test_subproject_loaded_names_the_file_and_its_parent(self):
        """Two deep: each event names the manifest that called envy.subproject."""
        self.write_sub()
        mid = self.tree / "mid"
        mid.mkdir()
        (mid / "envy.lua").write_text(
            'PACKAGES = envy.subproject("../sub").PACKAGES\n', encoding="utf-8"
        )
        top = self.tree / "envy.lua"
        top.write_text(
            '-- @envy bin "envy-bin"\n'
            f'-- @envy version "{PINNED_VERSION}"\n'
            'PACKAGES = envy.subproject("mid").PACKAGES\n',
            encoding="utf-8",
        )
        run = self.sync(top)

        self.assertEqual(0, run.returncode, run.stderr)
        parents = {
            Path(e.raw["path"]).resolve(): Path(e.raw["parent"]).resolve()
            for e in run.events("subproject_loaded")
        }
        self.assertEqual(
            {self.sub / "envy.lua": mid / "envy.lua", mid / "envy.lua": top}, parents
        )

    def test_discovery_never_resolves_the_subproject_manifest(self):
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
            "the subproject's bin dir must stay inert",
        )

    def _record_root(self) -> Path:
        """The subproject writes its envy.PROJECT_ROOT to a marker as it loads."""
        marker = self.make_temp_dir("markers") / "root"
        sub = self.write_sub()
        sub.write_text(
            sub.read_text(encoding="utf-8")
            + f'local f = assert(io.open("{self.lua_path(marker)}", "w"))\n'
            + "f:write(envy.PROJECT_ROOT)\nf:close()\n",
            encoding="utf-8",
        )
        return marker

    def test_a_subproject_sees_its_own_root_wherever_the_command_runs(self):
        """--subproject moves which project a command targets; never a subproject's root.

        From the top, from inside the subproject (the walk passes it: root "false"), and
        as the leaf, the same manifest must see the same envy.PROJECT_ROOT.
        """
        marker = self._record_root()
        top = self.write_super()
        for label, args, resolved in (
            ("top", (), top),
            ("inside", (), top),
            ("leaf", ("--subproject",), self.sub / "envy.lua"),
        ):
            with self.subTest(label):
                marker.unlink(missing_ok=True)
                run = self.run_envy(
                    "sync", *args, cwd=self.tree if label == "top" else self.sub
                )
                self.assertEqual(0, run.returncode, run.stderr)
                self.assertEqual(self.sub, Path(marker.read_text(encoding="utf-8")))
                [event] = run.events("manifest_resolved")
                self.assertEqual(resolved, Path(event.raw["path"]).resolve())

    def test_spec_phases_still_see_the_run_root(self):
        """A spec acts for the project being installed, wherever it was declared."""
        marker = self.make_temp_dir("markers") / "phase-root"
        (self.sub / "specs" / "subtool.lua").write_text(
            SUB_SPEC.replace(
                "function FETCH(tmp_dir, options) end",
                "function FETCH(tmp_dir, options)\n"
                f'  local f = assert(io.open("{self.lua_path(marker)}", "w"))\n'
                "  f:write(envy.PROJECT_ROOT)\n"
                "  f:close()\n"
                "end",
            ),
            encoding="utf-8",
        )
        self.write_sub()
        run = self.sync(self.write_super())

        self.assertEqual(0, run.returncode, run.stderr)
        self.assertEqual(self.tree, Path(marker.read_text(encoding="utf-8")))

    def _super_with(self, entries: str) -> Path:
        """A superproject declaring `entries` ahead of the subproject's own."""
        self.write_sub()
        manifest = self.tree / "envy.lua"
        manifest.write_text(
            '-- @envy bin "envy-bin"\n'
            f'-- @envy version "{PINNED_VERSION}"\n'
            'local sub = envy.subproject("sub")\n'
            f"PACKAGES = envy.extend({{\n{entries}}}, sub.PACKAGES)\n",
            encoding="utf-8",
        )
        return manifest

    def test_a_spec_the_subproject_only_hosts_installs_through_its_root(self):
        run = self.sync(
            self._super_with(
                '  { spec = "local.standalone@v1",\n'
                '    source = sub.PROJECT_ROOT .. "/specs/standalone.lua" },\n'
            )
        )

        self.assertEqual(0, run.returncode, run.stderr)
        self.assertTrue(self.pkg_complete("local.standalone@v1"))

    def test_one_spec_by_relative_and_project_root_paths_is_one_source(self):
        """The subproject's relative path and the superproject's spelling must agree."""
        run = self.sync(
            self._super_with(
                '  { spec = "local.subtool@v1",\n'
                '    source = sub.PROJECT_ROOT .. "/specs/subtool.lua" },\n'
            )
        )

        self.assertEqual(0, run.returncode, run.stderr)
        self.assertTrue(self.pkg_complete("local.subtool@v1"))

    def test_a_subproject_requiring_a_newer_envy_is_an_error(self):
        self.write_sub(version="99.0.0")
        run = self.sync(self.write_super())

        self.assertNotEqual(0, run.returncode, run.stdout)
        self.assertIn("requires envy 99.0.0", run.stderr)

    def test_a_subproject_pinning_an_older_envy_only_warns(self):
        self.write_sub(version=OLDER_VERSION)
        run = self.sync(self.write_super())

        self.assertEqual(0, run.returncode, run.stderr)
        self.assertIn(f"pins envy {OLDER_VERSION}", run.stderr)

    def _super_over(self, sub_extra: str) -> Path:
        """A superproject over a subproject that also declares `sub_extra`."""
        self.write_sub()
        sub = self.sub / "envy.lua"
        sub.write_text(sub.read_text(encoding="utf-8") + sub_extra, encoding="utf-8")
        return self.write_super()

    def test_a_subproject_default_shell_is_refused(self):
        """Only the root manifest's DEFAULT_SHELL is ever read; say so, do not drop it."""
        run = self.sync(
            self._super_over("DEFAULT_SHELL = ENVY_SHELL.SH\n")
        )

        self.assertNotEqual(0, run.returncode, run.stdout)
        self.assertIn("DEFAULT_SHELL", run.stderr)
        self.assertPathContains(run.stderr, "sub/envy.lua")

    def test_an_unadopted_subproject_depot_is_never_touched(self):
        """A depot only accelerates, so a subproject's PACKAGE_DEPOTS waits to be adopted.

        Both depot forms are tripwires: the URI hits a server that logs every request,
        and FETCH leaves a marker, then fails. The root's own depots, on the same
        witnesses, prove they would fire.
        """
        served = self.make_temp_dir("depots")
        (served / "root.txt").write_text("", encoding="utf-8")
        requests: list[str] = []
        base = self.serve_directory(served, requests=requests)
        markers = self.make_temp_dir("markers")
        poison_fetched, root_fetched = markers / "poison", markers / "root"

        manifest = self._super_over(
            "PACKAGE_DEPOTS = {\n"
            f'  "{base}/poison.txt",\n'
            "  { FETCH = function(ctx)\n"
            f'    io.open("{self.lua_path(poison_fetched)}", "w"):close()\n'
            '    error("poisoned depot FETCH ran")\n'
            "  end },\n"
            "}\n"
        )
        manifest.write_text(
            manifest.read_text(encoding="utf-8")
            + "PACKAGE_DEPOTS = {\n"
            f'  "{base}/root.txt",\n'
            "  { FETCH = function(ctx)\n"
            f'    io.open("{self.lua_path(root_fetched)}", "w"):close()\n'
            "    return {}\n"
            "  end },\n"
            "}\n",
            encoding="utf-8",
        )
        run = self.sync(manifest)

        self.assertEqual(0, run.returncode, run.stderr)
        self.assertTrue(self.pkg_complete("local.subtool@v1"))
        self.assertEqual(["/root.txt"], requests)
        self.assertTrue(root_fetched.exists(), "the root's own depot FETCH never ran")
        self.assertFalse(poison_fetched.exists(), "the subproject's depot FETCH ran")

    def test_a_conflict_between_two_subprojects_names_both_manifests(self):
        """Provenance is the file that wrote the entry, not the root that spliced it in.

        Two subprojects declare one bundle identity from different URLs. The root
        never mentions the bundle, so naming it would tell the author nothing.
        """
        for name, url in (("one", "https://example.com/a.zip"),
                          ("two", "https://example.com/b.zip")):
            sub = self.tree / name
            sub.mkdir()
            (sub / "envy.lua").write_text(
                '-- @envy bin "bin"\n'
                '-- @envy root "false"\n'
                "BUNDLES = {\n"
                f'  tc = {{ identity = "shared.tc@v1", source = "{url}" }},\n'
                "}\n"
                'PACKAGES = {\n'
                f'  {{ spec = "sub.{name}@v1", bundle = "tc" }},\n'
                "}\n",
                encoding="utf-8",
            )

        manifest = self.tree / "envy.lua"
        manifest.write_text(
            '-- @envy bin "envy-bin"\n'
            'PACKAGES = envy.extend(envy.subproject("one").PACKAGES,\n'
            '                       envy.subproject("two").PACKAGES)\n',
            encoding="utf-8",
        )
        run = self.sync(manifest)

        self.assertNotEqual(0, run.returncode, run.stdout)
        self.assertIn("conflicting sources", run.stderr)
        self.assertPathContains(run.stderr, "one/envy.lua")
        self.assertPathContains(run.stderr, "two/envy.lua")

    def test_a_missing_subproject_names_the_argument_and_the_resolved_path(self):
        manifest = self.tree / "envy.lua"
        manifest.write_text(
            '-- @envy bin "envy-bin"\nPACKAGES = envy.subproject("nope").PACKAGES\n',
            encoding="utf-8",
        )
        run = self.sync(manifest)

        self.assertNotEqual(0, run.returncode, run.stdout)
        self.assertIn("'nope' not found (resolved to ", run.stderr)


if __name__ == "__main__":
    unittest.main()
