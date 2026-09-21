"""Functional tests for envy.loadenv_bundle(): a manifest calling out of a bundle.

A bundle can carry one generic spec plus the entry builder that names it, so a
consumer writes a line per dependency instead of restating the spec, the alias and
the vendor path every time. The bundle therefore has to be materialized before the
manifest's global scope finishes -- earlier than the engine fetches bundles.
"""

from __future__ import annotations

import unittest
from pathlib import Path

from . import test_config
from .env import EnvyTestCase


# A spec with nothing to fetch: the tests are about reaching the helper beside it,
# not about what the helper's entries end up installing.
GENERIC_SPEC = """IDENTITY = "{identity}"
DEPENDENCIES = {{}}

USER_MANAGED = true
SETUP = {{
  main = {{
    CHECK = function(pkg_dir, options) return false end,
    INSTALL = function(pkg_dir, options) end,
  }},
}}
"""

# A cache-managed spec: only a package with a cached payload has anything to vendor.
VENDOR_SPEC = """IDENTITY = "{identity}"
FETCH = {{ source = "file://{seed}" }}
INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  envy.copy("{seed}", install_dir)
end
"""

# The shape the issue asks for: one builder, one line per dependency. `bundle`
# names the alias the consuming manifest declared, which is where it resolves.
HELPER = """local M = {}
function M.entry(name)
  return { spec = "test.generic@r1", bundle = "tools",
           options = { name = name }, setup = { "main" } }
end
return M
"""


class LoadenvBundleCase(EnvyTestCase):
    def setUp(self):
        super().setUp()
        self.project = self.make_temp_dir("project")
        self.seed = self.project / "seed.txt"
        self.seed.write_text("seed\n", encoding="utf-8")

    # -- authoring ----------------------------------------------------------

    def make_bundle(
        self,
        identity: str = "test.tools@r1",
        helper: str = HELPER,
        helper_path: str = "lib/github.lua",
        spec: str = GENERIC_SPEC,
        root: Path | None = None,
    ) -> Path:
        if root is None:
            root = self.make_temp_dir("bundle")
        root.mkdir(parents=True, exist_ok=True)
        (root / "envy-bundle.lua").write_text(
            f'BUNDLE = "{identity}"\n'
            'SPECS = { ["test.generic@r1"] = "specs/generic.lua" }\n',
            encoding="utf-8",
        )
        (root / "specs").mkdir(exist_ok=True)
        (root / "specs" / "generic.lua").write_text(
            spec.format(identity="test.generic@r1", seed=self.lua_path(self.seed)),
            encoding="utf-8",
        )
        path = root / helper_path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(helper, encoding="utf-8")
        return root

    def manifest(self, body: str, directory: Path | None = None) -> Path:
        path = (directory or self.project) / "envy.lua"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(test_config.make_manifest(body), encoding="utf-8")
        return path

    def bundles_table(self, root: Path | str, alias: str = "tools", **extra) -> str:
        source = root if isinstance(root, str) else self.lua_path(root)
        fields = [
            f'identity = "{extra.pop("identity", "test.tools@r1")}"',
            f'source = "{source}"',
        ]
        fields += [f'{k} = "{v}"' for k, v in extra.items()]
        return f"BUNDLES = {{\n  {alias} = {{ {', '.join(fields)} }},\n}}\n"

    def helper_manifest(self, root: Path, **extra) -> Path:
        return self.manifest(
            self.bundles_table(root, **extra)
            + 'local gh = envy.loadenv_bundle("tools", "lib.github")\n'
            "PACKAGES = {\n"
            '  gh.entry("one"),\n'
            '  gh.entry("two"),\n'
            "}\n"
        )


class TestLoadenvBundle(LoadenvBundleCase):
    def test_a_manifest_builds_its_packages_from_a_bundled_helper(self):
        run = self.install(self.helper_manifest(self.make_bundle()))

        self.assertEqual(0, run.returncode, run.stderr)
        self.assertEqual(2, len(run.events("pkg_outcome", spec="test.generic@r1")))
        self.assertTrue(self.spec_complete("test.tools@r1"))

    def test_a_local_bundle_is_read_where_it_stands(self):
        """`local.` bundles are used in situ, at manifest scope as anywhere else."""
        root = self.make_bundle(identity="local.tools@r1")
        run = self.install(self.helper_manifest(root, identity="local.tools@r1"))

        self.assertEqual(0, run.returncode, run.stderr)
        self.assertEqual([], self.spec_entries("local.tools@r1"))

    def test_a_git_bundle_is_cloned_before_the_manifest_finishes(self):
        repo = self.make_git_repo(
            {
                "envy-bundle.lua": 'BUNDLE = "test.tools@r1"\n'
                'SPECS = { ["test.generic@r1"] = "specs/generic.lua" }\n',
                "specs/generic.lua": GENERIC_SPEC.format(identity="test.generic@r1"),
                "lib/github.lua": HELPER,
            },
            label="bundlerepo",
        )
        head = self.git("rev-parse", "HEAD", cwd=repo).stdout.strip()
        manifest = self.manifest(
            "BUNDLES = {\n"
            f'  tools = {{ identity = "test.tools@r1", '
            f'source = "{self.lua_path(repo)}", ref = "{head}" }},\n'
            "}\n"
            'local gh = envy.loadenv_bundle("tools", "lib.github")\n'
            'PACKAGES = { gh.entry("one") }\n'
        )

        run = self.install(manifest)
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertEqual(1, len(run.events("pkg_outcome", spec="test.generic@r1")))

    def test_the_helper_may_hand_back_an_entry_that_vendors(self):
        """The builder's whole point is emitting `vendor` too; see issue #373."""
        root = self.make_bundle(
            spec=VENDOR_SPEC,
            helper="""local M = {}
function M.entry(name)
  return { spec = "test.generic@r1", bundle = "tools",
           options = { name = name }, vendor = "vendor/" .. name }
end
return M
"""
        )
        run = self.install(self.helper_manifest(root))
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertEqual(
            {"vendor/one", "vendor/two"},
            {
                Path(e.raw["path"]).relative_to(self.project).as_posix()
                for e in run.events("vendor_result")
            },
        )

    def test_a_module_that_assigns_globals_still_works(self):
        root = self.make_bundle(
            helper="""function entry(name)
  return { spec = "test.generic@r1", bundle = "tools",
           options = { name = name }, setup = { "main" } }
end
"""
        )
        run = self.install(self.helper_manifest(root))
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertEqual(2, len(run.events("pkg_outcome", spec="test.generic@r1")))

    def test_a_relative_source_in_a_fragment_anchors_on_the_fragment(self):
        sub = self.project / "sub"
        self.make_bundle(root=sub / "bundles" / "tools")
        (sub / "envy.lua").write_text(
            test_config.make_manifest(
                self.bundles_table("bundles/tools")
                + 'local gh = envy.loadenv_bundle("tools", "lib.github")\n'
                'PACKAGES = { gh.entry("one") }\n'
            ),
            encoding="utf-8",
        )
        manifest = self.manifest(
            'local sub = envy.import("sub")\nPACKAGES = sub.PACKAGES\n'
        )

        run = self.install(manifest)
        self.assertEqual(0, run.returncode, run.stderr)

    def test_a_root_alias_reached_from_a_fragment_anchors_on_the_root(self):
        """A fragment with no BUNDLES of its own sees the root's, which was written
        over there -- so its relative `source` resolves over there too, exactly as a
        literal `bundle = "tools"` entry in that fragment already does."""
        self.make_bundle(root=self.project / "bundles" / "tools")
        sub = self.project / "sub"
        sub.mkdir()
        (sub / "envy.lua").write_text(
            test_config.make_manifest(
                'local gh = envy.loadenv_bundle("tools", "lib.github")\n'
                'PACKAGES = { gh.entry("one") }\n'
            ),
            encoding="utf-8",
        )
        manifest = self.manifest(
            self.bundles_table("bundles/tools")
            + 'local sub = envy.import("sub")\nPACKAGES = sub.PACKAGES\n'
        )

        run = self.install(manifest)
        self.assertEqual(0, run.returncode, run.stderr)

    def test_a_fragment_alias_wins_over_the_root_one(self):
        self.make_bundle(root=self.project / "bundles" / "tools")  # never reached
        sub = self.project / "sub"
        self.make_bundle(root=sub / "bundles" / "tools", identity="test.own@r1")
        (sub / "envy.lua").write_text(
            test_config.make_manifest(
                self.bundles_table("bundles/tools", identity="test.own@r1")
                + 'local gh = envy.loadenv_bundle("tools", "lib.github")\n'
                'PACKAGES = { gh.entry("one") }\n'
            ),
            encoding="utf-8",
        )
        manifest = self.manifest(
            self.bundles_table("bundles/tools")
            + 'local sub = envy.import("sub")\nPACKAGES = sub.PACKAGES\n'
        )

        run = self.install(manifest)
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertTrue(self.spec_complete("test.own@r1"))
        self.assertEqual([], self.spec_entries("test.tools@r1"))

    def test_an_imported_fragment_resolves_its_own_alias(self):
        root = self.make_bundle()
        sub = self.project / "sub"
        sub.mkdir()
        (sub / "envy.lua").write_text(
            test_config.make_manifest(
                self.bundles_table(root)
                + 'local gh = envy.loadenv_bundle("tools", "lib.github")\n'
                'PACKAGES = { gh.entry("one") }\n'
            ),
            encoding="utf-8",
        )
        manifest = self.manifest(
            'local sub = envy.import("sub")\nPACKAGES = sub.PACKAGES\n'
        )

        run = self.install(manifest)
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertEqual(1, len(run.events("pkg_outcome", spec="test.generic@r1")))


class TestLoadenvBundleRefusals(LoadenvBundleCase):
    def test_an_unknown_alias_names_the_alias(self):
        manifest = self.manifest(
            self.bundles_table(self.make_bundle())
            + 'local gh = envy.loadenv_bundle("nope", "lib.github")\n'
            "PACKAGES = {}\n"
        )
        run = self.install(manifest)

        self.assertNotEqual(0, run.returncode)
        self.assertIn("nope", run.stderr)

    def test_an_alias_declared_after_the_call_is_not_found(self):
        """BUNDLES is a table, not a promise: the call reads what is assigned."""
        manifest = self.manifest(
            'local gh = envy.loadenv_bundle("tools", "lib.github")\n'
            + self.bundles_table(self.make_bundle())
            + "PACKAGES = {}\n"
        )
        run = self.install(manifest)

        self.assertNotEqual(0, run.returncode)
        self.assertIn("tools", run.stderr)

    def test_a_custom_fetch_bundle_is_refused_with_its_reason(self):
        manifest = self.manifest(
            "BUNDLES = {\n"
            "  tools = { identity = \"test.tools@r1\", source = { fetch = "
            "function(tmp_dir) end } },\n"
            "}\n"
            'local gh = envy.loadenv_bundle("tools", "lib.github")\n'
            "PACKAGES = {}\n"
        )
        run = self.install(manifest)

        self.assertNotEqual(0, run.returncode)
        self.assertIn("tools", run.stderr)
        self.assertIn("custom fetch", run.stderr)

    def test_a_missing_module_names_the_file(self):
        manifest = self.manifest(
            self.bundles_table(self.make_bundle())
            + 'local gh = envy.loadenv_bundle("tools", "lib.nothing")\n'
            "PACKAGES = {}\n"
        )
        run = self.install(manifest)

        self.assertNotEqual(0, run.returncode)
        self.assertIn("lib/nothing.lua", run.stderr.replace("\\", "/"))

    def test_a_module_path_that_is_not_dot_syntax_is_refused(self):
        manifest = self.manifest(
            self.bundles_table(self.make_bundle())
            + 'local gh = envy.loadenv_bundle("tools", "lib/github")\n'
            "PACKAGES = {}\n"
        )
        run = self.install(manifest)

        self.assertNotEqual(0, run.returncode)
        self.assertIn("dot syntax", run.stderr)

    def test_a_spec_is_pointed_at_loadenv_spec_instead(self):
        spec = self.write_spec(
            "consumer.lua",
            """IDENTITY = "local.consumer@v1"
local gh = envy.loadenv_bundle("tools", "lib.github")
USER_MANAGED = true
SETUP = { main = {
  CHECK = function(pkg_dir, options) return false end,
  INSTALL = function(pkg_dir, options) end,
} }
""",
            directory=self.project,
        )
        manifest = self.manifest(
            "PACKAGES = {\n"
            f'  {{ spec = "local.consumer@v1", source = "{self.lua_path(spec)}", '
            'setup = { "main" } },\n'
            "}\n"
        )
        run = self.install(manifest)

        self.assertNotEqual(0, run.returncode)
        self.assertIn("envy.loadenv_spec", run.stderr)


if __name__ == "__main__":
    unittest.main()
