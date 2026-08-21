"""A source.dependencies closure must use strong dependencies.

A fetch dependency is started with target=completion, so it runs its whole phase
ladder during graph resolution -- and every consumer above it is parked inside its
own spec_fetch waiting on an edge. `pending_spec_fetches_` counts every started task
until its own spec_fetch completes, so those parked consumers hold the count above
zero for exactly as long as the closure runs. `wait_for_resolution_phase()` -- hence
`resolve_weak_references()` -- cannot run in that window.

So a weak reference held by anything in a fetch closure resolves *after* its holder
has already finished. Before this was enforced, that was silent: the package built,
`needed_by` was violated, and the run exited 0. Measured, a closure member's BUILD
ran at seq 24 while the provider it declared for `needed_by = "build"` did not finish
installing until seq 106.

Scheduling cannot fix it, in either direction:

  - A weak entry *in* source.dependencies needs satisfaction at spec_fetch, but the
    pass runs only after every spec_fetch -- including its own consumer's.
  - A weak reference *held by* a closure member needs the pass, which needs the
    consumer's spec_fetch, which needs this member installed.

Both are circular, so the reference is rejected instead. Two checks, because a
package can enter a closure in either order relative to its own spec_fetch:
declaration-time (already marked, now declaring) and mark-time (already declared, now
marked). Without the second, whether you get an error would depend on which won the
race -- the same manifest failing loudly or miscompiling silently, run to run.
"""

from pathlib import Path

from .env import EnvyTestCase


PROVIDER_SPEC = """IDENTITY = "local.prov@v1"
PRODUCTS = { wk = "wk" }
FETCH = function(tmp_dir)
  local f = io.open(tmp_dir .. "/wk", "w") f:write("w") f:close()
  envy.commit_fetch("wk")
end
STAGE = function() end
INSTALL = function(install_dir, stage_dir, fetch_dir)
  envy.copy(fetch_dir .. "wk", install_dir .. "wk")
end
"""

BUNDLE_TAIL = r"""
        local b = io.open(tmp_dir .. "/envy-bundle.lua", "w")
        b:write('BUNDLE = "corp.specs@r1"\nSPECS = { ["corp.thing@r1"] = "thing.lua" }\n')
        b:close()
        local t = io.open(tmp_dir .. "/thing.lua", "w")
        t:write('IDENTITY = "corp.thing@r1"\nUSER_MANAGED = true\n' ..
                'SETUP = { m = { CHECK = function() return true end, ' ..
                'INSTALL = function() end } }\n')
        t:close()
        envy.commit_fetch({ "envy-bundle.lua", "thing.lua" })
"""


class TestFetchClosureWeakRefs(EnvyTestCase):
    def setUp(self):
        super().setUp()
        self.prov = self.write_spec("prov.lua", PROVIDER_SPEC)

    # -- fixtures -----------------------------------------------------------

    def closure_pkg(self, name: str, identity: str, dependencies: str) -> Path:
        """A cache-managed spec for use inside a fetch closure."""
        return self.write_spec(
            name,
            f"""IDENTITY = "{identity}"
DEPENDENCIES = {{ {dependencies} }}
FETCH = function(tmp_dir)
  local f = io.open(tmp_dir .. "/x", "w") f:write("x") f:close()
  envy.commit_fetch("x")
end
STAGE = function() end
BUILD = function(build_dir, stage_dir, fetch_dir) envy.info("BUILT {identity}") end
INSTALL = function(install_dir, stage_dir, fetch_dir)
  envy.copy(fetch_dir .. "x", install_dir .. "x")
end
""",
        )

    def bundle_manifest(self, fetch_deps: str, extra_packages: str = "") -> Path:
        """A manifest whose bundle takes `fetch_deps` as its source.dependencies."""
        return self.write_manifest(
            f"""
BUNDLES = {{
  corp = {{ identity = "corp.specs@r1",
    source = {{
      dependencies = {{ {fetch_deps} }},
      fetch = function(tmp_dir)
{BUNDLE_TAIL}
      end }} }},
}}
PACKAGES = {{
  {{ spec = "corp.thing@r1", bundle = "corp" }},
{extra_packages}
}}
"""
        )

    def prov_entry(self, needed_by: str = "build") -> str:
        return (
            f'{{ spec = "local.prov@v1", source = "{self.lua_path(self.prov)}", '
            f'needed_by = "{needed_by}" }}'
        )

    def prov_root(self) -> str:
        return f'  {{ spec = "local.prov@v1", source = "{self.lua_path(self.prov)}" }},'

    def assertRejected(self, run, *fragments):
        self.assertNotEqual(0, run.returncode, f"expected rejection\n{run.stderr}")
        for fragment in fragments:
            self.assertIn(fragment, run.stderr)

    # -- weak references held by closure members ----------------------------

    def test_weak_product_ref_in_closure_is_rejected(self):
        """A fetch dependency may not hold a weak product reference."""
        spec = self.closure_pkg(
            "p.lua", "local.p@v1", '{ product = "wk", needed_by = "build" }'
        )
        run = self.sync(
            self.bundle_manifest(
                f'{{ spec = "local.p@v1", source = "{self.lua_path(spec)}" }}',
                self.prov_root(),
            )
        )
        self.assertRejected(run, "must use strong dependencies", "wk")

    def test_weak_identity_ref_in_closure_is_rejected(self):
        """Reference-only entries are rejected on the same grounds as product ones."""
        spec = self.closure_pkg(
            "p.lua", "local.p@v1", '{ spec = "local.prov", needed_by = "build" }'
        )
        run = self.sync(
            self.bundle_manifest(
                f'{{ spec = "local.p@v1", source = "{self.lua_path(spec)}" }}',
                self.prov_root(),
            )
        )
        self.assertRejected(run, "must use strong dependencies", "local.prov")

    def test_transitive_closure_member_weak_ref_is_rejected(self):
        """The rejection covers the whole closure, not just its named entries.

        local.r@v1 is a *strong* dependency of the fetch dependency, so it is dragged
        through the ladder during resolution too -- measured at build seq 40 against a
        provider that installed at seq 143. Marking has to propagate transitively or
        this case stays silently broken.
        """
        self.closure_pkg("r.lua", "local.r@v1", '{ product = "wk", needed_by = "build" }')
        spec = self.closure_pkg(
            "p.lua", "local.p@v1", '{ spec = "local.r@v1", source = "r.lua" }'
        )
        run = self.sync(
            self.bundle_manifest(
                f'{{ spec = "local.p@v1", source = "{self.lua_path(spec)}" }}',
                self.prov_root(),
            )
        )
        self.assertRejected(run, "must use strong dependencies", "local.r@v1", "wk")

    def test_spec_declared_fetch_dep_closure_is_rejected(self):
        """The other acquisition path: source.dependencies on a DEPENDENCIES entry."""
        self.closure_pkg("p.lua", "local.p@v1", '{ product = "wk", needed_by = "build" }')
        parent = self.write_spec(
            "parent.lua",
            """IDENTITY = "local.parent@v1"
USER_MANAGED = true
DEPENDENCIES = {
  { spec = "gen.thing@v1",
    source = {
      dependencies = { { spec = "local.p@v1", source = "p.lua" } },
      fetch = function(tmp_dir)
        local f = io.open(tmp_dir .. "/spec.lua", "w")
        f:write('IDENTITY = "gen.thing@v1"\\nUSER_MANAGED = true\\n' ..
                'SETUP = { m = { CHECK = function() return true end, ' ..
                'INSTALL = function() end } }\\n')
        f:close()
        envy.commit_fetch("spec.lua")
      end } },
}
SETUP = { m = { CHECK = function() return true end, INSTALL = function() end } }
""",
        )
        run = self.sync(
            self.write_manifest(
                f"""
PACKAGES = {{
  {{ spec = "local.parent@v1", source = "{self.lua_path(parent)}" }},
{self.prov_root()}
}}
"""
            )
        )
        self.assertRejected(run, "must use strong dependencies", "wk")

    def test_package_entering_a_closure_after_its_own_spec_fetch_is_rejected(self):
        """The retroactive order: declared first, marked second.

        local.p@v1 is both a manifest root and the bundle's fetch dependency, so
        whether its spec_fetch completes before the bundle's on_start marks it is a
        race. Either order must be refused -- the declaration-time check catches one,
        the mark-time check the other -- so the *outcome* is deterministic even though
        which check fires is not. That is the point: without the second check the
        error itself would be racy.
        """
        spec = self.closure_pkg(
            "p.lua", "local.p@v1", '{ product = "wk", needed_by = "build" }'
        )
        run = self.sync(
            self.bundle_manifest(
                f'{{ spec = "local.p@v1", source = "{self.lua_path(spec)}" }}',
                self.prov_root() + f'\n  {{ spec = "local.p@v1", '
                f'source = "{self.lua_path(spec)}" }},',
            )
        )
        self.assertRejected(run, "strong dependencies", "wk")

    # -- weak entries in source.dependencies itself -------------------------

    def test_weak_fetch_prerequisite_is_rejected(self):
        """A reference-only source.dependencies entry cannot be ordered at all.

        Measured before enforcement: the consumer's fetch function ran at seq <=11
        while the entry's provider had not started its spec_fetch until seq 14.
        """
        run = self.sync(self.bundle_manifest('{ spec = "local.prov" }', self.prov_root()))
        self.assertRejected(run, "must be a strong reference")

    def test_weak_fetch_prerequisite_with_fallback_is_rejected(self):
        """The `weak = { ... }` fallback form is rejected on the same grounds."""
        run = self.sync(
            self.bundle_manifest(
                '{ spec = "local.helper", weak = { spec = "local.prov@v1", '
                f'source = "{self.lua_path(self.prov)}" }} }}',
                self.prov_root(),
            )
        )
        self.assertRejected(run, "must be a strong reference")

    # -- controls: what must keep working -----------------------------------

    def test_strong_closure_honors_needed_by(self):
        """An all-strong closure still works, and its ordering is actually enforced.

        The positive half of the fix: rejecting weak references is only correct if
        strong ones are genuinely ordered. Asserted on the trace rather than on exit
        status, because a silent violation is exactly what exits 0.
        """
        spec = self.closure_pkg("p.lua", "local.p@v1", self.prov_entry())
        run = self.sync(
            self.bundle_manifest(
                f'{{ spec = "local.p@v1", source = "{self.lua_path(spec)}" }}'
            )
        )

        self.assertEqual(0, run.returncode, run.stderr)
        prov_installed = [
            e.seq
            for e in run.events("phase_complete", spec="local.prov@v1")
            if e.raw.get("phase") == "install"
        ]
        p_build = [
            e.seq
            for e in run.events("phase_start", spec="local.p@v1")
            if e.raw.get("phase") == "build"
        ]
        self.assertTrue(prov_installed and p_build, run.stderr)
        self.assertLess(
            prov_installed[0],
            p_build[0],
            "needed_by=build not honored: closure member built before its dependency "
            "finished installing",
        )

    def test_root_package_weak_ref_still_resolves(self):
        """Outside a fetch closure, weak references are untouched.

        Guards against over-rejection. A plain root is capped at spec_fetch during
        resolution, so the pass runs before it proceeds and the reference is ordered
        normally -- which is why this case was never broken.
        """
        spec = self.closure_pkg(
            "p.lua", "local.p@v1", '{ product = "wk", needed_by = "build" }'
        )
        run = self.sync(
            self.write_manifest(
                f"""
PACKAGES = {{
  {{ spec = "local.p@v1", source = "{self.lua_path(spec)}" }},
{self.prov_root()}
}}
"""
            )
        )

        self.assertEqual(0, run.returncode, run.stderr)
        self.assertIn("BUILT local.p@v1", run.stderr)
        resolved = [e for e in run.events("product_resolved", spec="local.p@v1")]
        self.assertEqual(1, len(resolved), resolved)
        self.assertEqual("local.prov@v1", resolved[0].raw["provider"])
